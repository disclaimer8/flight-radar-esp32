#if defined(ARDUINO)
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <TFT_eSPI.h>
#include "config.h"
#include "flight_core.h"
#include "render_core.h"
#include "cst816s.h"
#include <NimBLEDevice.h>
#include "ble_core.h"

TFT_eSPI    tft = TFT_eSPI();
TFT_eSprite fb  = TFT_eSprite(&tft);   // full-screen 240x240 framebuffer
CST816S     touch(TOUCH_SDA, TOUCH_SCL, TOUCH_RST, TOUCH_INT);

static const int CX = 120, CY = 120, MAXR = 100;
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

static double rangeKm() { return RADIUS_NM * 1.852; }

void connectWifi() {
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);   // reconnect in the background; loop() never blocks on it
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
        delay(250); Serial.print(".");
    }
    Serial.println();
    Serial.println(WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString()
                                                  : "WiFi connect failed");
}

void pollApi() {
    // Called only when WiFi is connected (see loop()). No blocking reconnect here.

    char url[160];
    std::snprintf(url, sizeof(url),
        "https://api.airplanes.live/v2/point/%.4f/%.4f/%d",
        (double)MY_LAT, (double)MY_LON, (int)RADIUS_NM);

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
        g_cache = parseNearest(std::string(payload.c_str()), MY_LAT, MY_LON, MAX_AIRCRAFT, HIDE_GROUND_AIRCRAFT);
        if (g_idx >= g_cache.size()) g_idx = 0;
        g_centerLat = MY_LAT; g_centerLon = MY_LON;
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

    // blips
    for (size_t i = 0; i < g_cache.size(); i++) {
        const Aircraft& ac = g_cache[i];
        double b = bearingDeg(g_centerLat, g_centerLon, ac.lat, ac.lon);
        ScreenPoint p = polarToXY(b, ac.distKm, rangeKm(), CX, CY, MAXR);
        if (i == 0) {
            fb.fillCircle(p.x, p.y, 4, TFT_YELLOW);
            std::string cs = ac.callsign.empty() ? "------" : ac.callsign;
            fb.setTextDatum(TL_DATUM);
            fb.setTextColor(TFT_YELLOW, TFT_BLACK);
            fb.drawString(cs.c_str(), p.x + 6, p.y - 4, 2);
        } else {
            fb.fillCircle(p.x, p.y, 2, TFT_GREEN);
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
    fb.drawString(cs.c_str(), CX, 66, 4);

    double b = bearingDeg(g_centerLat, g_centerLon, ac.lat, ac.lon);
    std::string sub = (ac.type.empty() ? "----" : ac.type);
    sub += "  ";
    sub += compassPoint(b);
    fb.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    fb.drawString(sub.c_str(), CX, 96, 2);

    // Font 4 has the full ASCII set ("km"); font 6 is digits-only, so use 4 here.
    fb.setTextColor(TFT_YELLOW, TFT_BLACK);
    fb.drawString(fmtDist(ac.distKm).c_str(), CX, 128, 4);

    std::string row = fmtAlt(ac) + "   " + fmtSpeed(ac);
    fb.setTextColor(TFT_WHITE, TFT_BLACK);
    fb.drawString(row.c_str(), CX, 168, 2);

    // page-position dots, spacing shrunk so the row always fits ~180px wide
    int n = (int)g_cache.size();
    if (n > 1) {
        int spacing = 12;
        if ((n - 1) * spacing > 180) spacing = 180 / (n - 1);
        int startX = CX - (n - 1) * spacing / 2;
        for (int i = 0; i < n; i++) {
            uint16_t c = (i == (int)g_idx) ? TFT_CYAN : TFT_DARKGREY;
            fb.fillCircle(startX + i * spacing, 196, 2, c);
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
        if (g == TG_CLICK) { g_view = DETAIL; g_idx = 0; }
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
        BlePacket pkt = parseBlePacket(g_bleBuf, g_bleLen, MAX_AIRCRAFT);
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
