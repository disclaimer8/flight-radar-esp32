#if defined(ARDUINO)
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include "config.h"
#include "flight_core.h"

LiquidCrystal_I2C lcd(LCD_ADDR, 16, 2);

std::vector<Aircraft> g_cache;
size_t  g_cycleIdx = 0;
unsigned long g_lastPoll = 0;
unsigned long g_lastCycle = 0;
bool g_stale = false;

void lcdShow(const std::string& l1, const std::string& l2) {
    lcd.clear();
    lcd.setCursor(0, 0); lcd.print(l1.c_str());
    lcd.setCursor(0, 1); lcd.print(l2.c_str());
}

void i2cScan() {
    Serial.println("I2C scan:");
    for (byte a = 1; a < 127; a++) {
        Wire.beginTransmission(a);
        if (Wire.endTransmission() == 0) Serial.printf("  found @ 0x%02X\n", a);
    }
}

void connectWifi() {
    lcdShow("WiFi...", WIFI_SSID);
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
    if (WiFi.status() != WL_CONNECTED) { connectWifi(); return; }

    char url[160];
    std::snprintf(url, sizeof(url),
        "https://api.airplanes.live/v2/point/%.4f/%.4f/%d",
        (double)MY_LAT, (double)MY_LON, (int)RADIUS_NM);

    // Cloudflare 301-redirects http->https, so talk TLS directly. The API is
    // public read-only data; skip cert validation rather than pin a CA.
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
        g_cycleIdx = 0;
        g_stale = false;
        Serial.printf("poll ok: %u aircraft\n", (unsigned)g_cache.size());
    } else {
        g_stale = true;
        Serial.printf("poll failed: HTTP %d\n", code);
    }
    http.end();
}

void renderCurrent() {
    if (g_cache.empty()) {
        char l2[17];
        std::snprintf(l2, sizeof(l2), "in range %dkm", (int)std::lround(RADIUS_NM * 1.852));
        lcdShow("No aircraft", l2);
        return;
    }
    if (g_cycleIdx >= g_cache.size()) g_cycleIdx = 0;
    const Aircraft& ac = g_cache[g_cycleIdx];
    lcdShow(formatLine1(ac, g_stale), formatLine2(ac));
    g_cycleIdx++;
}

void setup() {
    Serial.begin(115200);
    Wire.begin(LCD_SDA, LCD_SCL);
    i2cScan();
    lcd.init();
    lcd.backlight();
    connectWifi();
    pollApi();
    renderCurrent();
    g_lastPoll = millis();
    g_lastCycle = millis();
}

void loop() {
    unsigned long now = millis();
    if (now - g_lastPoll >= POLL_INTERVAL_MS) {
        pollApi();
        g_lastPoll = now;
    }
    if (now - g_lastCycle >= CYCLE_INTERVAL_MS) {
        renderCurrent();
        g_lastCycle = now;
    }
}
#endif // ARDUINO
