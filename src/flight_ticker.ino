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
#include "cst816s.h"
#include <NimBLEDevice.h>
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

std::vector<Aircraft> g_cache;
unsigned long g_lastPoll  = 0;
unsigned long g_lastTouch = 0;
bool g_stale = false;

// Data source arbitration: Wi-Fi is primary; BLE (phone gateway) is the fallback.
enum Source { SRC_NONE, SRC_WIFI, SRC_BLE };
Source        g_source    = SRC_NONE;
double        g_centerLat = MY_LAT;   // radar center: config in Wi-Fi mode, packet GPS in BLE mode
double        g_centerLon = MY_LON;
unsigned long g_bleLastRx = 0;        // millis of last accepted BLE packet

enum View { RADAR, DETAIL };
View    g_view = RADAR;
size_t  g_idx  = 0;
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
// old fixed rangeKm(); the API reception radius is separate (see pollApi()).
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

void pollApi() {
    // Called only when WiFi is connected (see loop()). No blocking reconnect here.

    char url[160];
    std::snprintf(url, sizeof(url),
        "https://api.airplanes.live/v2/point/%.4f/%.4f/%d",
        g_obsLat, g_obsLon, queryRadiusNm(kRangePresets[kRangeCount - 1]));

    // Cloudflare 301-redirects http->https; talk TLS directly. Public read-only
    // data, so skip cert validation rather than pin a CA.
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.begin(client, url);
    http.setUserAgent("flight-ticker-esp32");
    http.setConnectTimeout(8000);
    http.setTimeout(8000);
    int code = http.GET();
    if (code == 200) {
        String payload = http.getString();
        g_cache = parseNearest(std::string(payload.c_str()), g_obsLat, g_obsLon, RADAR_PLOT_CAP, HIDE_GROUND_AIRCRAFT);
        if (g_idx >= g_cache.size()) g_idx = 0;
        g_centerLat = g_obsLat; g_centerLon = g_obsLon;
        g_stale = false;
        Serial.printf("poll ok: %u aircraft\n", (unsigned)g_cache.size());
    } else {
        g_stale = true;
        Serial.printf("poll failed: HTTP %d\n", code);
    }
    http.end();
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
    std::string rOrigin = ac.origin, rDest = ac.dest;
    if (rOrigin.empty() && WiFi.status() == WL_CONNECTED) {
        auto rt = lookupRoute(ac.callsign);
        rOrigin = rt.first; rDest = rt.second;
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
        }
    } else { // DETAIL
        if (g == TG_LEFT && !g_cache.empty()) {
            g_idx = (g_idx + 1) % g_cache.size();
        } else if (g == TG_RIGHT && !g_cache.empty()) {
            g_idx = (g_idx + g_cache.size() - 1) % g_cache.size();
        } else if (g == TG_CLICK || g == TG_DOWN) {
            g_view = RADAR;
        }
    }
}

void setup() {
    Serial.begin(115200);
    tft.init();
    tft.setRotation(0);
    tft.fillScreen(TFT_BLACK);
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
    bleSvc->start();
    NimBLEAdvertising* bleAdv = NimBLEDevice::getAdvertising();
    bleAdv->addServiceUUID(BLE_SERVICE_UUID);
    bleAdv->setName(BLE_DEVICE_NAME);
    bleAdv->start();

    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.drawString("WiFi...", CX, CY, 4);

    connectWifi();
    pollApi();
    g_lastPoll  = millis();
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
    // pollApi() blocks up to ~8s (HTTP timeout); the sweep freezes and touches are
    // ignored for that window. Acceptable on this single-threaded firmware — not a bug.
    if (now - g_lastPoll >= POLL_INTERVAL_MS) {
        if (WiFi.status() == WL_CONNECTED) pollApi();   // skip when offline; no blocking reconnect
        g_lastPoll = now;
    }

    // Source arbitration: Wi-Fi wins when connected; else BLE if fresh; else nothing.
    // g_bleLastRx != 0 gate keeps BLE dormant until a real packet arrives (at boot
    // now < BLE_FRESHNESS_MS would otherwise read as "fresh" with no data).
    if (WiFi.status() == WL_CONNECTED)                                  g_source = SRC_WIFI;
    else if (g_bleLastRx != 0 && now - g_bleLastRx <= BLE_FRESHNESS_MS) g_source = SRC_BLE;
    else                                                               g_source = SRC_NONE;

    handleTouch();
    if (g_view == DETAIL && now - g_lastTouch >= IDLE_RETURN_MS) g_view = RADAR;

    if (g_view == RADAR) drawRadar(); else drawDetail();
    delay(16); // ~60 fps cap
}
#endif // ARDUINO
