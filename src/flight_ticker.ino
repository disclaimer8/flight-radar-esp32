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

TFT_eSPI    tft = TFT_eSPI();
TFT_eSprite fb  = TFT_eSprite(&tft);   // full-screen 240x240 framebuffer
CST816S     touch(TOUCH_SDA, TOUCH_SCL, TOUCH_RST, TOUCH_INT);

static const int CX = 120, CY = 120, MAXR = 100;

std::vector<Aircraft> g_cache;
unsigned long g_lastPoll  = 0;
unsigned long g_lastTouch = 0;
bool g_stale = false;

enum View { RADAR, DETAIL };
View    g_view = RADAR;
size_t  g_idx  = 0;
uint8_t g_lastGesture = TG_NONE;

static double rangeKm() { return RADIUS_NM * 1.852; }

void connectWifi() {
    WiFi.mode(WIFI_STA);
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
    if (WiFi.status() != WL_CONNECTED) { connectWifi(); if (WiFi.status() != WL_CONNECTED) { g_stale = true; return; } }

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
        g_cache = parseNearest(std::string(payload.c_str()), MY_LAT, MY_LON, MAX_AIRCRAFT);
        if (g_idx >= g_cache.size()) g_idx = 0;
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
        double b = bearingDeg(MY_LAT, MY_LON, ac.lat, ac.lon);
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
    if (g_stale) fb.fillCircle(228, 12, 4, TFT_RED);

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

    double b = bearingDeg(MY_LAT, MY_LON, ac.lat, ac.lon);
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
    uint8_t g = touch.readGesture();
    if (g == g_lastGesture) return;   // edge-trigger: act once per gesture
    g_lastGesture = g;
    if (g == TG_NONE) return;
    g_lastTouch = millis();

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
    // pollApi() blocks up to ~8s (HTTP timeout), or ~20s during a WiFi reconnect;
    // the sweep freezes and touches are ignored for that window. Acceptable on this
    // single-threaded firmware — not a bug.
    if (now - g_lastPoll >= POLL_INTERVAL_MS) { pollApi(); g_lastPoll = now; }

    handleTouch();
    if (g_view == DETAIL && now - g_lastTouch >= IDLE_RETURN_MS) g_view = RADAR;

    if (g_view == RADAR) drawRadar(); else drawDetail();
    delay(16); // ~60 fps cap
}
#endif // ARDUINO
