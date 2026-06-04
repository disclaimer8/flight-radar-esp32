#if defined(ARDUINO)
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <TFT_eSPI.h>
#include "config.h"
#include "flight_core.h"
#include "render_core.h"
#include "coord_core.h"
#include "wifi_config_core.h"
#include "wifi_scan_core.h"
#include "photo_core.h"
#include "cst816s.h"
#include <NimBLEDevice.h>
#include <JPEGDEC.h>
#include "ble_core.h"
#include <map>
#include <Preferences.h>

TFT_eSPI    tft = TFT_eSPI();
TFT_eSprite fb  = TFT_eSprite(&tft);   // full-screen 240x240 framebuffer
CST816S     touch(TOUCH_SDA, TOUCH_SCL, TOUCH_RST, TOUCH_INT);

static const int CX = 120, CY = 120, MAXR = 100;
// Wi-Fi parse cap: keep more than the display cap so distant aircraft survive to
// be drawn as rim dots (the nearest 24 by distance). The detail carousel pages all.
static const int RADAR_PLOT_CAP = 24;

// Blip color per altBand() index: ground/unknown, <3k, 3-10k, 10-25k, 25-40k, >40k.
static const uint16_t kAltColors[6] = {
    TFT_DARKGREY, TFT_RED, TFT_ORANGE, TFT_GREENYELLOW, TFT_CYAN, TFT_BLUE
};
// The CST816S emits many INT events per physical touch (down/move/up), and latches
// the gesture across them. Collapse one touch into one action with a short cooldown.
static const unsigned long TOUCH_DEBOUNCE_MS = 300;

// Backlight dim: full on touch, ~30% after a minute idle (largest power draw).
static const unsigned long BL_DIM_MS = 60000;
static const uint8_t BL_FULL = 255, BL_DIM = 76;

std::vector<Aircraft> g_cache;
unsigned long g_lastPoll  = 0;
unsigned long g_lastTouch = 0;
bool g_stale = false;
uint8_t g_blLevel = 255;   // current backlight duty (avoid redundant ledcWrite)

// Data source arbitration: Wi-Fi is primary; BLE (phone gateway) is the fallback.
enum Source { SRC_NONE, SRC_WIFI, SRC_BLE };
Source        g_source    = SRC_NONE;
double        g_centerLat = MY_LAT;   // radar center: config in Wi-Fi mode, packet GPS in BLE mode
double        g_centerLon = MY_LON;
unsigned long g_bleLastRx = 0;        // millis of last accepted BLE packet

enum View { RADAR, DETAIL, PHOTO };
View    g_view = RADAR;
size_t  g_idx  = 0;
uint16_t*   g_photoPx = nullptr;   // cache-owned; valid only in PHOTO view
std::string g_photoCredit;
int g_rangeIdx = 1;  // index into kRangePresets; default 50 km. Restored from NVS in setup().
double g_obsLat = MY_LAT;  // observer location; default from config.h, restored from NVS in setup()
double g_obsLon = MY_LON;

// Touch INT latch: the CST816S pulses INT briefly on a touch event, too short to
// catch by polling the level each frame. A FALLING-edge ISR latches it; the loop
// reads the gesture once per event. Idle = no edge = no I2C, so the radar stays smooth.
volatile bool g_touchEvent = false;
void IRAM_ATTR onTouchISR() { g_touchEvent = true; }

// BLE ingest. The phone writes one binary packet (see ble_core.h) to this
// characteristic. The write callback (BLE task context) only copies bytes + sets
// a flag; loop() parses and updates g_cache, to avoid racing the render path.
static const char* BLE_DEVICE_NAME  = "FlightRadar";
static const char* BLE_SERVICE_UUID = "f1a90001-7e1d-4c2a-9b3f-1a2b3c4d5e6f";
static const char* BLE_CHAR_UUID    = "f1a90002-7e1d-4c2a-9b3f-1a2b3c4d5e6f";
static const char* BLE_WIFICFG_UUID = "f1a90003-7e1d-4c2a-9b3f-1a2b3c4d5e6f";
static const char* BLE_WIFISCAN_UUID = "f1a90004-7e1d-4c2a-9b3f-1a2b3c4d5e6f";

static uint8_t  g_bleBuf[BLE_MAX_PACKET];
volatile size_t g_bleLen = 0;
volatile bool   g_blePacketReady = false;

class IngestCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c) override {
        std::string v = c->getValue();
        size_t n = v.size();
        if (n > BLE_MAX_PACKET) n = BLE_MAX_PACKET;
        std::memcpy(g_bleBuf, v.data(), n);
        g_bleLen = n;
        g_blePacketReady = true;
    }
};

static uint8_t  g_wifiCfgBuf[128];
volatile size_t g_wifiCfgLen = 0;
volatile bool   g_wifiCfgReady = false;
NimBLECharacteristic* g_wifiCfgChar = nullptr;

// Receives a Wi-Fi provisioning packet from the app. Like IngestCallbacks, the
// write callback only buffers + flags; loop() does the apply (off the BLE task).
class WifiConfigCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c) override {
        std::string v = c->getValue();
        size_t n = v.size();
        if (n > sizeof(g_wifiCfgBuf)) n = sizeof(g_wifiCfgBuf);
        std::memcpy(g_wifiCfgBuf, v.data(), n);
        g_wifiCfgLen = n;
        g_wifiCfgReady = true;
    }
};

NimBLECharacteristic* g_wifiScanChar = nullptr;
volatile bool g_wifiScanRequested = false;
bool g_wifiScanInFlight = false;   // loop()-only; not shared with BLE task

// Scan-request write: like the other callbacks, only set a flag; loop() runs
// the (async) scan and notifies results off the BLE task.
class WifiScanCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c) override {
        std::string v = c->getValue();
        if (isScanRequest(reinterpret_cast<const uint8_t*>(v.data()), v.size()))
            g_wifiScanRequested = true;
    }
};

// ---- Aircraft photo pipeline (PHOTO view) ----------------------------------
// Decoded 240x240 RGB565 photos live in PSRAM (2 MB, otherwise idle): 8 LRU
// slots ≈ 920 KB + a transient download buffer. SRAM budget is untouched.
struct PhotoSlot {
    std::string key;            // registration, or hex when reg is empty
    uint16_t*   px = nullptr;   // 240*240 RGB565 in PSRAM; null = free slot
    std::string photographer;
    unsigned long lastUse = 0;
};
static const int PHOTO_CACHE_SLOTS = 8;
// planespotters 403s generic UAs — must be descriptive with a contact URL.
static const char* PHOTO_UA =
    "flight-radar-esp32/1.0 (+https://github.com/disclaimer8/flight-radar-esp32)";
PhotoSlot g_photoCache[PHOTO_CACHE_SLOTS];
std::map<std::string, bool> g_photoMiss;  // negative cache (known no-photo), per boot

// Persistent download scratch: avoids interleaving transient 150 KB
// allocations with the permanent 115 KB photo slots (PSRAM fragmentation).
static const size_t PHOTO_DL_MAX = 150 * 1024;
uint8_t* g_photoDlBuf = nullptr;   // lazily allocated once, never freed

// Forward declaration: lookupRoute is defined after the photo pipeline globals.
// netTask calls it on core 0 (its private cache); loop() must never call it.
std::pair<std::string, std::string> lookupRoute(const std::string& callsign);

// Forward declaration: fetchPhoto is defined after the photo pipeline globals.
// netTask calls it on core 0 (its private cache); loop() must never call it.
PhotoResult fetchPhoto(const Aircraft& ac);

// ---- netTask: all outbound HTTP on core 0 (render loop stays on core 1) ----
// Hand-off = the house volatile-flag pattern: netTask is the only writer of
// each result buffer, sets the ready flag LAST; loop() consumes at one safe
// point and clears the flag. Internal SRAM is uncached and per-core stores
// land in order, same assumption the BLE->loop path has always made.

// Poll channel: netTask parses into its scratch, then swap-publishes here.
std::vector<Aircraft> g_pollBuf;
volatile bool g_pollReady = false;

// Route channel: loop posts a callsign, netTask resolves (its private cache /
// hexdb) and publishes. Fixed char buffers — std::string must not cross cores.
char          g_routeReqKey[12] = "";
volatile bool g_routeReq = false;      // loop sets, netTask clears
char          g_routeResKey[12] = "";  // written LAST by netTask
char          g_routeResOrigin[8] = "";
char          g_routeResDest[8] = "";

// Photo channel: loop posts reg/hex, netTask runs the fetch/decode pipeline
// (its private cache) and publishes the cache-owned pixel pointer + credit.
char          g_photoReqReg[12] = "";
char          g_photoReqHex[8] = "";
volatile bool g_photoReq = false;      // loop sets, netTask clears
uint16_t*     g_photoResPx = nullptr;
char          g_photoResCredit[48] = "";
volatile bool g_photoResOk = false;
volatile bool g_photoReady = false;    // written LAST by netTask

bool          g_photoLoading = false;          // PHOTO view: request in flight
unsigned long g_photoMsgUntil = 0;             // non-blocking "No photo" deadline

void netPoll() {
    // Persistent keep-alive client: handshake once (~1.5 s), then each poll
    // is a bare GET (~200 ms). On any failure tear down so the next cycle
    // re-handshakes from scratch.
    static WiFiClientSecure s_client;
    static HTTPClient s_http;
    static bool s_init = false;
    if (!s_init) {
        s_client.setInsecure();   // public read-only data; no CA pinning
        s_http.setReuse(true);
        s_init = true;
    }
    // Snapshot the observer location once per cycle: loop() rewrites these
    // doubles on portal save, and a 64-bit store isn't atomic across cores.
    // A torn read would only mis-center one poll (self-heals next cycle);
    // the snapshot at least guarantees the URL and the parse use one pair.
    double obsLat = g_obsLat, obsLon = g_obsLon;
    char url[160];
    std::snprintf(url, sizeof(url),
        "https://api.airplanes.live/v2/point/%.4f/%.4f/%d",
        obsLat, obsLon, queryRadiusNm(kRangePresets[kRangeCount - 1]));
    s_http.begin(s_client, url);
    s_http.setUserAgent("flight-ticker-esp32");
    s_http.setConnectTimeout(8000);
    s_http.setTimeout(8000);
    int code = s_http.GET();
    if (code == 200) {
        String payload = s_http.getString();
        s_http.end();   // setReuse keeps the socket alive
        std::vector<Aircraft> fresh = parseNearest(
            std::string(payload.c_str()), obsLat, obsLon,
            RADAR_PLOT_CAP, HIDE_GROUND_AIRCRAFT);
        Serial.printf("poll ok: %u aircraft\n", (unsigned)fresh.size());
        if (!g_pollReady) {            // loop consumed the previous batch
            g_pollBuf.swap(fresh);
            g_pollReady = true;        // flag last
        }
    } else {
        Serial.printf("poll failed: %d\n", code);
        s_http.end();
        s_client.stop();               // force a clean handshake next cycle
        g_stale = true;                // single-writer note: see Step 3
    }
}

void netTask(void*) {
    unsigned long lastPoll = 0;
    for (;;) {
        if (WiFi.status() == WL_CONNECTED &&
            millis() - lastPoll >= POLL_INTERVAL_MS) {
            lastPoll = millis();
            netPoll();
        }
        if (g_routeReq) {
            char key[12];
            strlcpy(key, (const char*)g_routeReqKey, sizeof(key));
            auto rt = lookupRoute(key);                     // netTask-private cache
            strlcpy(g_routeResOrigin, rt.first.c_str(), sizeof(g_routeResOrigin));
            strlcpy(g_routeResDest, rt.second.c_str(), sizeof(g_routeResDest));
            strlcpy(g_routeResKey, key, sizeof(g_routeResKey));   // key last = result complete
            g_routeReq = false;
        }
        if (g_photoReq) {
            Aircraft ac;
            ac.registration = (const char*)g_photoReqReg;
            ac.hex          = (const char*)g_photoReqHex;
            PhotoResult r = fetchPhoto(ac);    // netTask-private cache/decoder
            g_photoResPx = r.px;
            strlcpy(g_photoResCredit, r.photographer.c_str(), sizeof(g_photoResCredit));
            g_photoResOk = r.ok;
            g_photoReady = true;               // flag last
            g_photoReq = false;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

JPEGDEC g_jpeg;
// JPEGDEC delivers MCU blocks via callback; these route them into the target
// framebuffer with the centering crop offsets (negative = letterbox margin).
uint16_t* g_decTarget = nullptr;
int g_decOffX = 0, g_decOffY = 0;

int photoDrawCb(JPEGDRAW* d) {
    for (int row = 0; row < d->iHeight; row++) {
        int ty = d->y + row - g_decOffY;
        if (ty < 0 || ty >= 240) continue;
        // iWidth is the buffer stride; iWidthUsed excludes stale columns on
        // right-edge MCU blocks (visible in the letterboxed case).
        for (int col = 0; col < d->iWidthUsed; col++) {
            int tx = d->x + col - g_decOffX;
            if (tx < 0 || tx >= 240) continue;
            g_decTarget[ty * 240 + tx] = d->pPixels[row * d->iWidth + col];
        }
    }
    return 1;
}

// netTask-PRIVATE from here on: loop() must never call lookupRoute or read
// g_routeCache (std::map across cores = UB).
std::map<std::string, std::pair<std::string, std::string>> g_routeCache; // callsign -> (origin,dest)

// Blocking hexdb.io route lookup; caches by callsign (incl. empties to avoid
// re-hitting). Returns (origin,dest) or ("",""). Call only when WiFi is connected.
std::pair<std::string, std::string> lookupRoute(const std::string& callsign) {
    if (callsign.empty()) return {"", ""};
    auto it = g_routeCache.find(callsign);
    if (it != g_routeCache.end()) return it->second;
    std::pair<std::string, std::string> result{"", ""};
    char url[96];
    std::snprintf(url, sizeof(url), "https://hexdb.io/api/v1/route/icao/%s", callsign.c_str());
    WiFiClientSecure client; client.setInsecure();
    HTTPClient http; http.begin(client, url);
    http.setUserAgent("flight-ticker-esp32");
    http.setConnectTimeout(2500); http.setTimeout(2500);
    if (http.GET() == 200) {
        JsonDocument doc;
        if (!deserializeJson(doc, http.getString()) && doc["route"].is<const char*>()) {
            result = parseHexdbRoute(std::string(doc["route"].as<const char*>()));
        }
    }
    http.end();
    g_routeCache[callsign] = result;
    return result;
}

// Persist the selected range index to NVS so the zoom survives reboot. Called only
// on a user-driven change (not per frame), so flash wear is negligible.
void saveRangeIdx() {
    Preferences prefs;
    prefs.begin("radar", false);   // read-write
    prefs.putInt("rangeIdx", g_rangeIdx);
    prefs.end();
}

// Persist the observer location to NVS (written by the Wi-Fi setup portal).
void saveLocation(double lat, double lon) {
    Preferences prefs;
    prefs.begin("radar", false);   // read-write
    prefs.putDouble("lat", lat);
    prefs.putDouble("lon", lon);
    prefs.end();
}

// Current display range (outer ring) in km, selected by touch zoom. Replaces the
// old fixed rangeKm(); the API reception radius is separate (see netPoll()).
static double displayRangeKm() { return kRangePresets[g_rangeIdx]; }

// The LCD screen shown while the Wi-Fi setup portal is open.
void drawSetupScreen() {
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.drawString("SETUP", CX, 70, 4);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString("Join Wi-Fi:", CX, 110, 2);
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.drawString("FlightRadar-Setup", CX, 132, 2);
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.drawString("then open 192.168.4.1", CX, 160, 2);
}

// Wire the lat/lon parameters + portal callbacks onto a WiFiManager. The two
// WiFiManagerParameters are owned by the caller (they must outlive the portal).
void setupPortalParams(WiFiManager& wm, WiFiManagerParameter& latParam,
                       WiFiManagerParameter& lonParam) {
    wm.addParameter(&latParam);
    wm.addParameter(&lonParam);
    wm.setAPCallback([](WiFiManager*) { drawSetupScreen(); });
    wm.setSaveParamsCallback([&latParam, &lonParam]() {
        double la, lo;
        if (parseLatLon(latParam.getValue(), lonParam.getValue(), la, lo)) {
            g_obsLat = la;
            g_obsLon = lo;
            saveLocation(la, lo);
        }
    });
}

void connectWifi() {
    WiFiManager wm;
    wm.setConfigPortalTimeout(180);   // 3 min, then boot offline (BLE fallback)

    char latBuf[16], lonBuf[16];
    std::snprintf(latBuf, sizeof(latBuf), "%.4f", g_obsLat);
    std::snprintf(lonBuf, sizeof(lonBuf), "%.4f", g_obsLon);
    WiFiManagerParameter latParam("lat", "Observer latitude", latBuf, 15);
    WiFiManagerParameter lonParam("lon", "Observer longitude", lonBuf, 15);
    setupPortalParams(wm, latParam, lonParam);

    // Seed: with no stored credentials, persist config.h creds so autoConnect
    // tries them before falling back to the portal.
    if (WiFi.SSID().isEmpty() && strlen(WIFI_SSID) > 0) {
        WiFi.persistent(true);
        WiFi.begin(WIFI_SSID, WIFI_PASS);
    }

    WiFi.setAutoReconnect(true);
    wm.autoConnect("FlightRadar-Setup");
    Serial.println(WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString()
                                                 : "WiFi not connected");
}

// Reopen the setup portal on demand (long-press). Blocks loop() while active.
void startPortalOnDemand() {
    WiFiManager wm;
    wm.setConfigPortalTimeout(180);
    char latBuf[16], lonBuf[16];
    std::snprintf(latBuf, sizeof(latBuf), "%.4f", g_obsLat);
    std::snprintf(lonBuf, sizeof(lonBuf), "%.4f", g_obsLon);
    WiFiManagerParameter latParam("lat", "Observer latitude", latBuf, 15);
    WiFiManagerParameter lonParam("lon", "Observer longitude", lonBuf, 15);
    setupPortalParams(wm, latParam, lonParam);
    drawSetupScreen();
    wm.startConfigPortal("FlightRadar-Setup");
}

void drawRadar() {
    fb.fillSprite(TFT_BLACK);

    // range rings + crosshair
    for (int k = 1; k <= 3; k++) fb.drawCircle(CX, CY, MAXR * k / 3, TFT_DARKGREEN);
    fb.drawFastVLine(CX, CY - MAXR, 2 * MAXR, 0x0320);
    fb.drawFastHLine(CX - MAXR, CY, 2 * MAXR, 0x0320);

    // rotating sweep with a fading trail
    double sweepDeg = (double)(millis() % SWEEP_PERIOD_MS) / SWEEP_PERIOD_MS * 360.0;
    for (int t = 0; t < 30; t++) {
        double a  = sweepDeg - t * 2.0;
        double th = a * M_PI / 180.0;
        int ex = CX + (int)(MAXR * sin(th));
        int ey = CY - (int)(MAXR * cos(th));
        uint16_t shade = (t == 0) ? TFT_GREEN
                                  : tft.color565(0, (uint8_t)max(0, 60 - t * 2), (uint8_t)max(0, 30 - t));
        fb.drawLine(CX, CY, ex, ey, shade);
    }

    // blips: in-range keep altitude color + heading vector (nearest gets a white
    // ring + label); aircraft beyond the display range render as small grey rim
    // dots at their bearing. Emergencies are detected regardless of range.
    bool blinkOn = (millis() / 500) % 2 == 0;
    bool anyEmergency = false;
    int  emergencyCode = 0;
    double dr = displayRangeKm();
    for (size_t i = 0; i < g_cache.size(); i++) {
        const Aircraft& ac = g_cache[i];
        double b = bearingDeg(g_centerLat, g_centerLon, ac.lat, ac.lon);
        ScreenPoint p = polarToXY(b, ac.distKm, dr, CX, CY, MAXR);

        bool emerg = isEmergencySquawk(ac.squawk);
        if (emerg) { anyEmergency = true; emergencyCode = ac.squawk; }

        if (isOnRim(ac.distKm, dr)) {
            uint16_t rc = (emerg && blinkOn) ? TFT_RED : TFT_DARKGREY;
            fb.fillCircle(p.x, p.y, 1, rc);
            continue;
        }

        uint16_t color = kAltColors[altBand(ac.altFt, ac.onGround)];
        if (emerg) color = blinkOn ? TFT_RED : TFT_DARKGREY;

        if (!std::isnan(ac.track)) {
            ScreenPoint e = vectorEnd(p, ac.track, 10.0);
            fb.drawLine(p.x, p.y, e.x, e.y, color);
        }

        if (i == 0) {
            fb.fillCircle(p.x, p.y, 4, color);
            fb.drawCircle(p.x, p.y, 6, TFT_WHITE); // nearest ring (in-range only)
            std::string cs = ac.callsign.empty() ? "------" : ac.callsign;
            fb.setTextDatum(TL_DATUM);
            fb.setTextColor(TFT_WHITE, TFT_BLACK);
            fb.drawString(cs.c_str(), p.x + 8, p.y - 4, 2);
        } else {
            fb.fillCircle(p.x, p.y, 2, color);
        }
    }

    // observer + labels
    fb.fillCircle(CX, CY, 2, TFT_WHITE);
    fb.setTextDatum(TC_DATUM);
    fb.setTextColor(TFT_GREEN, TFT_BLACK);
    fb.drawString("N", CX, 4, 2);
    if (g_cache.empty()) {
        fb.setTextColor(TFT_DARKGREY, TFT_BLACK);
        fb.drawString("NO TRAFFIC", CX, CY + 8, 2);
    }
    if (anyEmergency && blinkOn) {
        char ebuf[20];
        snprintf(ebuf, sizeof(ebuf), "EMERGENCY %d", emergencyCode);
        fb.setTextDatum(TC_DATUM);
        fb.setTextColor(TFT_RED, TFT_BLACK);
        fb.drawString(ebuf, CX, CY - 40, 2);
    }
    // Source indicator at bottom-center (inside the round panel):
    //   green W = Wi-Fi, red W = Wi-Fi poll failing, cyan B = BLE/phone, red NO LINK = no data.
    fb.setTextDatum(BC_DATUM);
    if (g_source == SRC_WIFI) {
        fb.setTextColor(g_stale ? TFT_RED : TFT_GREEN, TFT_BLACK);
        fb.drawString("W", CX, 236, 2);
    } else if (g_source == SRC_BLE) {
        fb.setTextColor(TFT_CYAN, TFT_BLACK);
        fb.drawString("B", CX, 236, 2);
    } else {
        fb.setTextColor(TFT_RED, TFT_BLACK);
        fb.drawString("NO LINK", CX, 236, 2);
    }

    // Range readout: names the outer-ring distance / current zoom. Top-center under
    // the "N" (corner positions are off the round GC9A01's visible glass).
    char rbuf[12];
    std::snprintf(rbuf, sizeof(rbuf), "%dkm", (int)dr);
    fb.setTextDatum(TC_DATUM);
    fb.setTextColor(TFT_DARKGREEN, TFT_BLACK);
    fb.drawString(rbuf, CX, 22, 2);

    fb.pushSprite(0, 0);
}

void drawDetail() {
    if (g_cache.empty()) { g_view = RADAR; drawRadar(); return; }
    if (g_idx >= g_cache.size()) g_idx = 0;
    const Aircraft& ac = g_cache[g_idx];

    fb.fillSprite(TFT_BLACK);
    fb.setTextDatum(MC_DATUM);

    std::string cs = ac.callsign.empty() ? "------" : ac.callsign;
    fb.setTextColor(TFT_CYAN, TFT_BLACK);
    fb.drawString(cs.c_str(), CX, 54, 4);

    double b = bearingDeg(g_centerLat, g_centerLon, ac.lat, ac.lon);
    std::string sub = (ac.type.empty() ? "----" : ac.type);
    sub += "  ";
    sub += compassPoint(b);
    if (ac.squawk != 0) { sub += "  "; sub += std::to_string(ac.squawk); }
    fb.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    fb.drawString(sub.c_str(), CX, 80, 2);

    // Font 4 has the full ASCII set ("km"); font 6 is digits-only, so use 4 here.
    fb.setTextColor(TFT_YELLOW, TFT_BLACK);
    fb.drawString(fmtDist(ac.distKm).c_str(), CX, 106, 4);

    std::string row = fmtAlt(ac) + "   " + fmtSpeed(ac);
    fb.setTextColor(TFT_WHITE, TFT_BLACK);
    fb.drawString(row.c_str(), CX, 132, 2);

    // Reg / Op / Route block. Route origin/dest comes from the BLE packet when
    // present, else a lazy (cached) hexdb.io lookup on Wi-Fi. TC_DATUM so each
    // line draws downward from its y; rows below the fields, above the dots.
    // Route comes from the netTask mailbox — never block the render loop on
    // hexdb. Until the result lands, the row shows a "..." placeholder.
    std::string rOrigin = ac.origin, rDest = ac.dest;
    bool routePending = false;
    if (rOrigin.empty() && WiFi.status() == WL_CONNECTED && !ac.callsign.empty()) {
        if (ac.callsign == (const char*)g_routeResKey) {
            rOrigin = (const char*)g_routeResOrigin;
            rDest   = (const char*)g_routeResDest;
        } else if (!g_routeReq) {
            strlcpy(g_routeReqKey, ac.callsign.c_str(), sizeof(g_routeReqKey));
            g_routeReq = true;         // flag last
            routePending = true;
        } else {
            routePending = true;       // a request (this or another) is in flight
        }
    }
    fb.setTextDatum(TC_DATUM);
    fb.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    if (!ac.registration.empty())
        fb.drawString(("Reg " + ac.registration).c_str(), CX, 150, 2);
    std::string op = airlineCode(ac.callsign);
    if (!op.empty())
        fb.drawString(("Op " + op).c_str(), CX, 168, 2);
    if (!rOrigin.empty() && rOrigin != rDest)
        fb.drawString((rOrigin + " > " + rDest).c_str(), CX, 186, 2);
    else if (routePending)
        fb.drawString("...", CX, 186, 2);

    // page-position dots, spacing shrunk so the row always fits ~180px wide
    int n = (int)g_cache.size();
    if (n > 1) {
        int spacing = 12;
        if ((n - 1) * spacing > 180) spacing = 180 / (n - 1);
        int startX = CX - (n - 1) * spacing / 2;
        for (int i = 0; i < n; i++) {
            uint16_t c = (i == (int)g_idx) ? TFT_CYAN : TFT_DARKGREY;
            fb.fillCircle(startX + i * spacing, 210, 2, c);
        }
    }

    fb.pushSprite(0, 0);
}

// Blocking sections (photo fetch, message flash) accumulate touch-INT edges
// from the same gesture (finger lift, CST816S event bursts); drop the latched
// flag and restart the debounce window so stale events don't replay. Don't
// touch the I2C bus here — the CST816S sleeps between touches and a read
// outside an INT window fails with Wire error -1.
void drainTouch() {
    g_touchEvent = false;
    g_lastTouch = millis();
}

// Centered one-liner shown for ~1.2 s (blocking — same one-shot acceptance as
// the photo fetch itself), then the next frame redraws the current view.
void flashPhotoMsg(const char* msg) {
    fb.fillSprite(TFT_BLACK);
    fb.setTextDatum(MC_DATUM);
    fb.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    fb.drawString(msg, CX, CY, 2);
    fb.pushSprite(0, 0);
    delay(1200);
}

// Swipe-up handler in DETAIL: post a photo request to netTask and switch view.
// The "No Wi-Fi" path still blocks 1.2 s (flashPhotoMsg) and drains touch;
// the success path is non-blocking — netTask does the fetch while loop() renders.
void enterPhotoView() {
    if (g_cache.empty()) return;
    if (g_idx >= g_cache.size()) g_idx = 0;
    const Aircraft& ac = g_cache[g_idx];
    if (WiFi.status() != WL_CONNECTED) {
        flashPhotoMsg("No Wi-Fi");
        drainTouch();
        return;
    }
    // Same guard as the route channel: while a fetch is in flight netTask may
    // be reading the request buffers — never overwrite them mid-read. The user
    // just re-swipes once the previous fetch drains (≤3 s).
    if (g_photoReq) return;
    strlcpy(g_photoReqReg, ac.registration.c_str(), sizeof(g_photoReqReg));
    strlcpy(g_photoReqHex, ac.hex.c_str(), sizeof(g_photoReqHex));
    g_photoReq = true;
    g_photoLoading = true;
    g_photoPx = nullptr;
    g_photoMsgUntil = 0;
    g_view = PHOTO;
}

void drawPhoto() {
    if (!g_photoPx) {
        if (g_photoMsgUntil != 0) {                 // failed: flash then return
            if (millis() >= g_photoMsgUntil) {
                g_photoMsgUntil = 0;
                g_view = DETAIL; drawDetail(); return;
            }
            fb.fillSprite(TFT_BLACK);
            fb.setTextDatum(MC_DATUM);
            fb.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
            fb.drawString("No photo", CX, CY, 2);
            fb.pushSprite(0, 0);
            return;
        }
        if (g_photoLoading) {                       // request in flight
            fb.fillSprite(TFT_BLACK);
            fb.setTextDatum(MC_DATUM);
            fb.setTextColor(TFT_CYAN, TFT_BLACK);
            fb.drawString("Loading photo...", CX, CY, 2);
            fb.pushSprite(0, 0);
            return;
        }
        g_view = DETAIL; drawDetail(); return;      // shouldn't happen; safe out
    }
    // JPEGDEC emits little-endian RGB565; pushImage wants swapped bytes.
    // (If hardware smoke shows red/blue swapped, flip this to false.)
    fb.setSwapBytes(true);
    fb.pushImage(0, 0, 240, 240, g_photoPx);
    fb.setSwapBytes(false);
    if (g_idx >= g_cache.size()) g_idx = 0;
    if (!g_cache.empty()) {
        const Aircraft& ac = g_cache[g_idx];
        std::string cs = ac.callsign.empty() ? "------" : ac.callsign;
        fb.setTextDatum(TC_DATUM);
        fb.setTextColor(TFT_WHITE, TFT_BLACK);
        fb.drawString(cs.c_str(), CX, 28, 2);
    }
    if (!g_photoCredit.empty()) {
        // GLCD font 1 has no '©'; "(c)" keeps the required attribution ASCII.
        fb.setTextDatum(BC_DATUM);
        fb.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
        fb.drawString(("(c) " + g_photoCredit + " / planespotters.net").c_str(),
                      CX, 214, 1);
    }
    fb.pushSprite(0, 0);
}

void handleTouch() {
    // Only touch the I2C bus when the ISR latched an INT edge (a real touch event).
    // Idle: no edge -> no bus read -> radar stays smooth. One edge per touch also
    // re-arms repeated same-direction swipes cleanly.
    if (!g_touchEvent) return;
    g_touchEvent = false;
    unsigned long now = millis();
    if (now - g_lastTouch < TOUCH_DEBOUNCE_MS) return;  // ignore the rest of this touch's event burst
    uint8_t g = touch.readGesture();
    if (g == TG_NONE) return;
    g_lastTouch = now;

    if (g_view == RADAR) {
        if (g == TG_CLICK) {
            g_view = DETAIL; g_idx = 0;
        } else if (g == TG_UP) {               // zoom in (smaller range)
            int n = clampRangeIndex(g_rangeIdx, -1, kRangeCount);
            if (n != g_rangeIdx) { g_rangeIdx = n; saveRangeIdx(); }
        } else if (g == TG_DOWN) {              // zoom out (larger range)
            int n = clampRangeIndex(g_rangeIdx, +1, kRangeCount);
            if (n != g_rangeIdx) { g_rangeIdx = n; saveRangeIdx(); }
        } else if (g == TG_LONG) {              // long-press: reopen Wi-Fi setup portal
            startPortalOnDemand();
        }
    } else if (g_view == DETAIL) {
        if (g == TG_LEFT && !g_cache.empty()) {
            g_idx = (g_idx + 1) % g_cache.size();
        } else if (g == TG_RIGHT && !g_cache.empty()) {
            g_idx = (g_idx + g_cache.size() - 1) % g_cache.size();
        } else if (g == TG_UP) {
            enterPhotoView();
        } else if (g == TG_CLICK || g == TG_DOWN) {
            g_view = RADAR;
        }
    } else { // PHOTO: any touch returns to the detail page
        g_view = DETAIL;
        g_photoPx = nullptr;   // cache still owns the pixels
        g_photoLoading = false;   // a late netTask result will be discarded
        g_photoMsgUntil = 0;
    }
}

// Notify the app of provisioning status: 1 code byte + ASCII detail (IP / reason).
void notifyWifiStatus(uint8_t code, const String& detail) {
    if (!g_wifiCfgChar) return;
    uint8_t buf[64];
    buf[0] = code;
    size_t dlen = detail.length();
    if (dlen > sizeof(buf) - 1) dlen = sizeof(buf) - 1;
    std::memcpy(buf + 1, detail.c_str(), dlen);
    g_wifiCfgChar->setValue(buf, 1 + dlen);
    g_wifiCfgChar->notify();
}

// Send scan results to the app: one notify per network (each fits any MTU),
// or a single total=0 notify when nothing was found / the scan failed.
// Blocks loop() ~20 ms per network (≤300 ms total) — a deliberate one-shot
// action, far below the accepted poll (~8 s) / provisioning (~12 s) blocks.
void sendScanResults(const std::vector<ScanNet>& nets) {
    if (!g_wifiScanChar) return;
    uint8_t buf[WIFISCAN_REC_MAX];
    if (nets.empty()) {
        g_wifiScanChar->setValue(buf, encodeScanEmpty(buf));
        g_wifiScanChar->notify();
        return;
    }
    uint8_t total = static_cast<uint8_t>(nets.size());
    for (uint8_t i = 0; i < total; i++) {
        size_t len = encodeScanRecord(buf, total, i, nets[i]);
        if (!len) continue;
        g_wifiScanChar->setValue(buf, len);
        g_wifiScanChar->notify();
        delay(20);   // pace notifies so the central's queue keeps up
    }
}

// netTask-PRIVATE from here on: loop() must never call fetchPhoto, httpsGetToBuf,
// or read g_photoCache, g_photoMiss, g_jpeg, g_photoDlBuf (photo pipeline is
// owned by netTask on core 0; cross-core access is UB for the map + data race
// for the PSRAM buffers).

// Download a URL into a caller-supplied buffer. Same TLS pattern as the hexdb
// lookup; requires Content-Length (planespotters sends it) and caps at maxLen.
// Writes into buf (must be at least maxLen bytes) and sets *outLen on success.
// Returns true on success; never allocates or frees memory.
bool httpsGetToBuf(const char* url, uint8_t* buf, size_t maxLen, size_t* outLen) {
    WiFiClientSecure client; client.setInsecure();
    HTTPClient http; http.begin(client, url);
    http.setUserAgent(PHOTO_UA);
    http.setConnectTimeout(2500); http.setTimeout(4000);
    if (http.GET() != 200) { http.end(); return false; }
    int len = http.getSize();
    if (len <= 0 || (size_t)len > maxLen) { http.end(); return false; }
    WiFiClient* s = http.getStreamPtr();
    size_t got = 0;
    unsigned long t0 = millis();
    while (got < (size_t)len && millis() - t0 < 8000) {
        int n = s->read(buf + got, (size_t)len - got);
        if (n > 0) got += (size_t)n; else delay(10);
    }
    http.end();
    if (got != (size_t)len) return false;
    *outLen = got;
    return true;
}

// LRU-insert a decoded photo. Eviction can never free the on-screen photo:
// fetches only happen on PHOTO entry, and the entering photo gets the
// freshest lastUse.
//
// Eviction invariant: inserts/evictions happen only while a request is
// outstanding; while a request is outstanding the loop displays "Loading"
// (g_photoPx == nullptr); therefore eviction can never free a displayed photo.
void photoCacheInsert(const std::string& key, uint16_t* px, const std::string& photographer) {
    PhotoSlot* slot = nullptr;
    for (auto& s : g_photoCache) if (!s.px) { slot = &s; break; }
    if (!slot) {
        slot = &g_photoCache[0];
        for (auto& s : g_photoCache) if (s.lastUse < slot->lastUse) slot = &s;
        heap_caps_free(slot->px);
    }
    slot->key = key; slot->px = px;
    slot->photographer = photographer; slot->lastUse = millis();
}

// Blocking lookup+fetch+decode (~1-3 s) — deliberate one-shot user action,
// same acceptance as the 12 s provisioning block. Cache hits return instantly.
PhotoResult fetchPhoto(const Aircraft& ac) {
    PhotoResult res;
    std::string key = !ac.registration.empty() ? ac.registration : ac.hex;
    if (key.empty()) return res;

    for (auto& s : g_photoCache) {
        if (s.px && s.key == key) {
            s.lastUse = millis();
            res.ok = true; res.px = s.px; res.photographer = s.photographer;
            return res;
        }
    }
    if (g_photoMiss.count(key)) return res;

    char url[160];
    const char* kind = !ac.registration.empty() ? "reg" : "hex";
    std::snprintf(url, sizeof(url),
                  "https://api.planespotters.net/pub/photos/%s/%s", kind, key.c_str());
    // The API endpoint responds CHUNKED over HTTP/1.1 (no Content-Length), so
    // httpsGetToPsram's getSize() path can't read it. getString() de-chunks,
    // and the body is tiny (~0.5-3 KB for one aircraft); only the image fetch
    // below needs the PSRAM streaming path (the CDN does send Content-Length).
    int code;
    String jsonBody;
    {
        WiFiClientSecure client; client.setInsecure();
        HTTPClient http; http.begin(client, url);
        http.setUserAgent(PHOTO_UA);
        http.setConnectTimeout(2500); http.setTimeout(4000);
        code = http.GET();
        if (code == 200) jsonBody = http.getString();
        http.end();
    }
    if (code != 200) return res;  // transient/HTTP error: retry next entry
    PsPhoto meta = parsePlanespottersPhoto(
        std::string(jsonBody.c_str(), jsonBody.length()));
    if (!meta.ok) { g_photoMiss[key] = true; return res; }  // confirmed no photo

    if (!g_photoDlBuf) {
        g_photoDlBuf = (uint8_t*)heap_caps_malloc(PHOTO_DL_MAX, MALLOC_CAP_SPIRAM);
        if (!g_photoDlBuf) return res;
    }
    size_t ilen = 0;
    // Via the re-encoding proxy: planespotters thumbs are progressive JPEGs,
    // which JPEGDEC can't really decode (see buildProxiedPhotoUrl).
    std::string imgUrl = buildProxiedPhotoUrl(meta.url);
    if (!httpsGetToBuf(imgUrl.c_str(), g_photoDlBuf, PHOTO_DL_MAX, &ilen)) return res;  // transient network failure: don't negative-cache

    uint16_t* px = (uint16_t*)heap_caps_malloc(240 * 240 * 2, MALLOC_CAP_SPIRAM);
    if (!px) return res;
    std::memset(px, 0, 240 * 240 * 2);  // black letterbox margins

    if (g_jpeg.openRAM(g_photoDlBuf, (int)ilen, photoDrawCb)) {
        int d = pickJpegScale(g_jpeg.getWidth(), g_jpeg.getHeight());
        int opt = (d == 8) ? JPEG_SCALE_EIGHTH
                : (d == 4) ? JPEG_SCALE_QUARTER
                : (d == 2) ? JPEG_SCALE_HALF : 0;
        g_decTarget = px;
        g_decOffX = cropOffset(g_jpeg.getWidth() / d);
        g_decOffY = cropOffset(g_jpeg.getHeight() / d);
        g_jpeg.setPixelType(RGB565_LITTLE_ENDIAN);
        int ok = g_jpeg.decode(0, 0, opt);
        g_jpeg.close();
        g_decTarget = nullptr;
        if (ok) {
            photoCacheInsert(key, px, meta.photographer);
            res.ok = true; res.px = px; res.photographer = meta.photographer;
        } else {
            heap_caps_free(px);
            g_photoMiss[key] = true;  // undecodable image: don't retry each tap
        }
    } else {
        heap_caps_free(px);
        g_photoMiss[key] = true;
    }
    return res;
}

// Apply a received Wi-Fi provisioning packet: parse, join, persist, report.
// Bounded blocking wait (~12s) is acceptable for a deliberate one-shot action.
void applyWifiConfig() {
    WifiConfig cfg = parseWifiConfig(g_wifiCfgBuf, g_wifiCfgLen);
    if (!cfg.ok) { notifyWifiStatus(2, "bad config"); return; }
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.drawString("Configuring", CX, 100, 4);
    tft.drawString("Wi-Fi...", CX, 140, 4);
    notifyWifiStatus(0, "");                 // applying
    WiFi.persistent(true);
    WiFi.begin(cfg.ssid.c_str(), cfg.pass.c_str());
    unsigned long t = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t < 12000) delay(100);
    if (WiFi.status() == WL_CONNECTED) {
        notifyWifiStatus(1, WiFi.localIP().toString());
    } else {
        notifyWifiStatus(2, "connect failed");
    }
}

void setup() {
    Serial.begin(115200);
    tft.init();
    tft.setRotation(0);
    tft.fillScreen(TFT_BLACK);
    // Take over the backlight pin with LEDC PWM (core 2.x API).
    ledcSetup(0, 5000, 8);          // channel 0, 5 kHz, 8-bit
    ledcAttachPin(TFT_BL, 0);
    ledcWrite(0, BL_FULL);
    fb.setColorDepth(16);
    if (!fb.createSprite(240, 240)) Serial.println("sprite alloc failed");
    touch.begin();
    // Restore the saved display range (default 50 km = index 1) + observer location.
    {
        Preferences prefs;
        prefs.begin("radar", true);    // read-only
        int saved = prefs.getInt("rangeIdx", 1);
        g_obsLat = prefs.getDouble("lat", MY_LAT);
        g_obsLon = prefs.getDouble("lon", MY_LON);
        prefs.end();
        if (saved < 0) saved = 0;
        if (saved > kRangeCount - 1) saved = kRangeCount - 1;
        g_rangeIdx = saved;
        g_centerLat = g_obsLat;
        g_centerLon = g_obsLon;
    }
    attachInterrupt(digitalPinToInterrupt(TOUCH_INT), onTouchISR, FALLING);

    NimBLEDevice::init(BLE_DEVICE_NAME);
    NimBLEDevice::setMTU(517);
    NimBLEServer* bleServer = NimBLEDevice::createServer();
    NimBLEService* bleSvc = bleServer->createService(BLE_SERVICE_UUID);
    NimBLECharacteristic* bleCh = bleSvc->createCharacteristic(
        BLE_CHAR_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    bleCh->setCallbacks(new IngestCallbacks());
    g_wifiCfgChar = bleSvc->createCharacteristic(
        BLE_WIFICFG_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::NOTIFY);
    g_wifiCfgChar->setCallbacks(new WifiConfigCallbacks());
    g_wifiScanChar = bleSvc->createCharacteristic(
        BLE_WIFISCAN_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::NOTIFY);
    g_wifiScanChar->setCallbacks(new WifiScanCallbacks());
    bleSvc->start();
    NimBLEAdvertising* bleAdv = NimBLEDevice::getAdvertising();
    bleAdv->addServiceUUID(BLE_SERVICE_UUID);
    bleAdv->setName(BLE_DEVICE_NAME);
    bleAdv->start();

    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.drawString("WiFi...", CX, CY, 4);

    connectWifi();
    // First poll comes from netTask within ~2 s of boot.
    xTaskCreatePinnedToCore(netTask, "net", 12288, nullptr, 1, nullptr, 0);
    g_lastPoll = millis();
    g_lastTouch = millis();
}

void loop() {
    unsigned long now = millis();
    if (g_blePacketReady) {
        g_blePacketReady = false;
        BlePacket pkt = parseBlePacket(g_bleBuf, g_bleLen, MAX_AIRCRAFT, HIDE_GROUND_AIRCRAFT);
        if (pkt.ok) {
            g_cache     = pkt.aircraft;
            g_centerLat = pkt.centerLat;
            g_centerLon = pkt.centerLon;
            if (g_idx >= g_cache.size()) g_idx = 0;
            g_bleLastRx = millis();
        }
    }
    if (g_pollReady) {
        g_cache.swap(g_pollBuf);
        g_pollBuf.clear();
        g_pollReady = false;           // netTask may publish again now
        if (g_idx >= g_cache.size()) g_idx = 0;
        g_centerLat = g_obsLat; g_centerLon = g_obsLon;
        g_stale = false;
        g_lastPoll = millis();         // freshness = when applied
    }
    if (g_photoReady) {
        g_photoReady = false;
        if (g_view == PHOTO && g_photoLoading) {
            g_photoLoading = false;
            if (g_photoResOk) {
                g_photoPx = g_photoResPx;
                g_photoCredit = (const char*)g_photoResCredit;
            } else {
                g_photoMsgUntil = millis() + 1500;   // "No photo", then back
            }
        }
        // else: user already left PHOTO — discard the late result.
    }
    if (g_wifiCfgReady) {
        g_wifiCfgReady = false;
        applyWifiConfig();
    }
    if (g_wifiScanRequested && !g_wifiScanInFlight) {
        g_wifiScanRequested = false;
        // Single-radio caveat: scanning while STA is connected goes off-channel,
        // which can briefly stall an in-flight HTTPS poll (auto-reconnect recovers).
        WiFi.scanNetworks(/*async=*/true);   // blocking scan would freeze the radar 2-3 s
        g_wifiScanInFlight = true;
    }
    if (g_wifiScanInFlight) {
        int n = WiFi.scanComplete();
        if (n >= 0) {
            std::vector<ScanNet> nets;
            for (int i = 0; i < n; i++) {
                ScanNet s;
                s.ssid = std::string(WiFi.SSID(i).c_str());
                int rssi = WiFi.RSSI(i);
                s.rssi = static_cast<int8_t>(rssi < -128 ? -128 : (rssi > 127 ? 127 : rssi));
                s.secured = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
                nets.push_back(s);
            }
            WiFi.scanDelete();
            sendScanResults(dedupSortCap(nets));
            g_wifiScanInFlight = false;
        } else if (n == WIFI_SCAN_FAILED) {
            sendScanResults({});
            g_wifiScanInFlight = false;
        }
        // n == WIFI_SCAN_RUNNING (-1): keep waiting
    }
    // Polling lives on netTask (core 0); g_lastPoll is bumped at consume time
    // above and only feeds the staleness indicator now.

    // Source arbitration: Wi-Fi wins when connected; else BLE if fresh; else nothing.
    // g_bleLastRx != 0 gate keeps BLE dormant until a real packet arrives (at boot
    // now < BLE_FRESHNESS_MS would otherwise read as "fresh" with no data).
    if (WiFi.status() == WL_CONNECTED)                                  g_source = SRC_WIFI;
    else if (g_bleLastRx != 0 && now - g_bleLastRx <= BLE_FRESHNESS_MS) g_source = SRC_BLE;
    else                                                               g_source = SRC_NONE;

    handleTouch();
    // Fresh millis() here: handleTouch can block for seconds (photo fetch),
    // making the loop-start `now` OLDER than g_lastTouch — the unsigned
    // subtraction then wraps to ~2^32 and fires the idle return instantly.
    if (g_view != RADAR && millis() - g_lastTouch >= IDLE_RETURN_MS) {
        g_view = RADAR;
        g_photoPx = nullptr;
        g_photoLoading = false;   // a late netTask result will be discarded
        g_photoMsgUntil = 0;
    }

    // Backlight: dim after a minute of no touches, restore instantly on touch.
    uint8_t want = (millis() - g_lastTouch >= BL_DIM_MS) ? BL_DIM : BL_FULL;
    if (want != g_blLevel) { g_blLevel = want; ledcWrite(0, want); }

    if (g_view == RADAR) drawRadar();
    else if (g_view == DETAIL) drawDetail();
    else drawPhoto();
    delay(16); // ~60 fps cap
}
#endif // ARDUINO
