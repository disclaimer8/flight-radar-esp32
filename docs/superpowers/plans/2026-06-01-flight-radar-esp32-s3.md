# Flight Radar (ESP32-S3 round touch) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Render a North-up aircraft radar on the Waveshare ESP32-S3-Touch-LCD-1.28, with a tap-to-open detail carousel of the nearest flights.

**Architecture:** Reuse `flight_core.h` (poll/parse/sort/distance) unchanged. Add a pure, host-tested `render_core.h` (bearing, polar projection, compass, field formatting). Rewrite `flight_ticker.ino` to drive a GC9A01 via TFT_eSPI (full-screen sprite framebuffer) plus a tiny CST816S touch driver and a RADAR↔DETAIL state machine.

**Tech Stack:** PlatformIO (ESP32-S3 Arduino + native Unity), TFT_eSPI, ArduinoJson, custom CST816S I2C driver.

**On-screen labels are Latin** (built-in TFT_eSPI fonts); Cyrillic deferred (would need a bundled custom font).

---

## File Structure

- `src/render_core.h` — **new.** Arduino-free pure math/formatting: `bearingDeg`, `polarToXY`, `compassPoint`, `fmtDist`, `fmtAlt`, `fmtSpeed`. Host-tested.
- `src/cst816s.h` — **new.** Minimal Arduino I2C driver for the CST816S touch gesture register.
- `src/flight_ticker.ino` — **rewrite.** WiFi/HTTP (reused) + TFT_eSPI sprite rendering + touch + state machine.
- `src/flight_core.h` — **unchanged.** (`formatLine1/2` stay, now unused by firmware.)
- `src/config.example.h` / `src/config.h` — **modify.** Drop LCD pins + `CYCLE_INTERVAL_MS`; add touch pins, `IDLE_RETURN_MS`, `SWEEP_PERIOD_MS`; bump `MAX_AIRCRAFT`.
- `platformio.ini` — **modify.** Replace `esp32dev` env with `esp32-s3` (TFT_eSPI build flags, PSRAM, native USB). `native` env unchanged.
- `test/test_core/test_main.cpp` — **modify.** Add tests for the new pure functions.

---

## Task 1: `render_core.h` — bearing

**Files:**
- Create: `src/render_core.h`
- Test: `test/test_core/test_main.cpp`

- [ ] **Step 1: Write the failing tests**

Add to `test/test_core/test_main.cpp` after the existing includes (top of file, after line 2):

```cpp
#include "../../src/render_core.h"
```

Add these test functions before `void setUp`:

```cpp
void test_bearing_cardinals(void) {
    // From equator origin: north=0, east=90, south=180, west=270
    TEST_ASSERT_FLOAT_WITHIN(0.5, 0.0,   bearingDeg(0.0, 0.0,  1.0,  0.0));
    TEST_ASSERT_FLOAT_WITHIN(0.5, 90.0,  bearingDeg(0.0, 0.0,  0.0,  1.0));
    TEST_ASSERT_FLOAT_WITHIN(0.5, 180.0, bearingDeg(0.0, 0.0, -1.0,  0.0));
    TEST_ASSERT_FLOAT_WITHIN(0.5, 270.0, bearingDeg(0.0, 0.0,  0.0, -1.0));
}

void test_bearing_normalized_range(void) {
    double b = bearingDeg(48.0, 11.0, 47.5, 10.5); // southwest-ish
    TEST_ASSERT_TRUE(b >= 0.0 && b < 360.0);
    TEST_ASSERT_TRUE(b > 180.0 && b < 270.0);
}
```

Register them in `main()` (after the last existing `RUN_TEST`):

```cpp
    RUN_TEST(test_bearing_cardinals);
    RUN_TEST(test_bearing_normalized_range);
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `/opt/homebrew/bin/pio test -e native -f test_core`
Expected: FAIL — compile error, `render_core.h` not found / `bearingDeg` undefined.

- [ ] **Step 3: Create `src/render_core.h` with `bearingDeg`**

```cpp
#pragma once
#include <cmath>
#include <cstdio>
#include <string>
#include "flight_core.h"   // Aircraft, ftToM, ktToKmh

// Initial great-circle bearing observer->target, degrees, north=0, clockwise, [0,360).
inline double bearingDeg(double lat1, double lon1, double lat2, double lon2) {
    const double toRad = M_PI / 180.0;
    double dLon = (lon2 - lon1) * toRad;
    double y = std::sin(dLon) * std::cos(lat2 * toRad);
    double x = std::cos(lat1 * toRad) * std::sin(lat2 * toRad) -
               std::sin(lat1 * toRad) * std::cos(lat2 * toRad) * std::cos(dLon);
    double b = std::atan2(y, x) * 180.0 / M_PI;
    return std::fmod(b + 360.0, 360.0);
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `/opt/homebrew/bin/pio test -e native -f test_core`
Expected: PASS — all tests green (19 now).

- [ ] **Step 5: Commit**

```bash
git add src/render_core.h test/test_core/test_main.cpp
git commit -m "feat: add bearingDeg to render_core with tests"
```

---

## Task 2: `render_core.h` — polar projection

**Files:**
- Modify: `src/render_core.h`
- Test: `test/test_core/test_main.cpp`

- [ ] **Step 1: Write the failing tests**

Add before `void setUp`:

```cpp
void test_polar_center_at_zero_distance(void) {
    ScreenPoint p = polarToXY(123.0, 0.0, 50.0, 120, 120, 96);
    TEST_ASSERT_EQUAL_INT(120, p.x);
    TEST_ASSERT_EQUAL_INT(120, p.y);
}

void test_polar_north_and_east_full_range(void) {
    ScreenPoint n = polarToXY(0.0, 50.0, 50.0, 120, 120, 96); // due north = up
    TEST_ASSERT_EQUAL_INT(120, n.x);
    TEST_ASSERT_EQUAL_INT(24,  n.y);
    ScreenPoint e = polarToXY(90.0, 50.0, 50.0, 120, 120, 96); // due east = right
    TEST_ASSERT_EQUAL_INT(216, e.x);
    TEST_ASSERT_EQUAL_INT(120, e.y);
}

void test_polar_clamps_beyond_range(void) {
    ScreenPoint far = polarToXY(90.0, 999.0, 50.0, 120, 120, 96); // 999km > 50km range
    TEST_ASSERT_EQUAL_INT(216, far.x); // pinned to ring edge
    TEST_ASSERT_EQUAL_INT(120, far.y);
}
```

Register in `main()`:

```cpp
    RUN_TEST(test_polar_center_at_zero_distance);
    RUN_TEST(test_polar_north_and_east_full_range);
    RUN_TEST(test_polar_clamps_beyond_range);
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `/opt/homebrew/bin/pio test -e native -f test_core`
Expected: FAIL — `ScreenPoint` / `polarToXY` undefined.

- [ ] **Step 3: Add `ScreenPoint` + `polarToXY` to `src/render_core.h`** (after `bearingDeg`)

```cpp
struct ScreenPoint { int x; int y; };

// Map (bearing, distance) to screen pixels. North up, screen Y grows downward.
// Distance is clamped to [0, rangeKm]; radius scales linearly to maxRadiusPx.
inline ScreenPoint polarToXY(double bearing, double distKm, double rangeKm,
                             int cx, int cy, int maxRadiusPx) {
    double d = distKm;
    if (d < 0) d = 0;
    if (d > rangeKm) d = rangeKm;
    double r = (rangeKm > 0) ? (d / rangeKm) * maxRadiusPx : 0.0;
    double th = bearing * M_PI / 180.0;
    ScreenPoint p;
    p.x = (int)std::lround(cx + r * std::sin(th));
    p.y = (int)std::lround(cy - r * std::cos(th));
    return p;
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `/opt/homebrew/bin/pio test -e native -f test_core`
Expected: PASS (22 tests green).

- [ ] **Step 5: Commit**

```bash
git add src/render_core.h test/test_core/test_main.cpp
git commit -m "feat: add polarToXY projection with tests"
```

---

## Task 3: `render_core.h` — compass points

**Files:**
- Modify: `src/render_core.h`
- Test: `test/test_core/test_main.cpp`

- [ ] **Step 1: Write the failing tests**

Add before `void setUp`:

```cpp
void test_compass_cardinals(void) {
    TEST_ASSERT_EQUAL_STRING("N",  compassPoint(0.0));
    TEST_ASSERT_EQUAL_STRING("NE", compassPoint(45.0));
    TEST_ASSERT_EQUAL_STRING("E",  compassPoint(90.0));
    TEST_ASSERT_EQUAL_STRING("S",  compassPoint(180.0));
    TEST_ASSERT_EQUAL_STRING("W",  compassPoint(270.0));
    TEST_ASSERT_EQUAL_STRING("NW", compassPoint(315.0));
}

void test_compass_wraps_and_rounds(void) {
    TEST_ASSERT_EQUAL_STRING("N", compassPoint(359.0)); // wraps to N
    TEST_ASSERT_EQUAL_STRING("N", compassPoint(10.0));  // rounds to N
    TEST_ASSERT_EQUAL_STRING("NE", compassPoint(30.0)); // rounds to NE
}
```

Register in `main()`:

```cpp
    RUN_TEST(test_compass_cardinals);
    RUN_TEST(test_compass_wraps_and_rounds);
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `/opt/homebrew/bin/pio test -e native -f test_core`
Expected: FAIL — `compassPoint` undefined.

- [ ] **Step 3: Add `compassPoint` to `src/render_core.h`**

```cpp
// 8-point compass rose label for a bearing in degrees.
inline const char* compassPoint(double bearing) {
    static const char* pts[8] = {"N","NE","E","SE","S","SW","W","NW"};
    double b = std::fmod(bearing + 360.0, 360.0);
    int idx = ((int)std::lround(b / 45.0)) % 8;
    return pts[idx];
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `/opt/homebrew/bin/pio test -e native -f test_core`
Expected: PASS (24 tests green).

- [ ] **Step 5: Commit**

```bash
git add src/render_core.h test/test_core/test_main.cpp
git commit -m "feat: add compassPoint with tests"
```

---

## Task 4: `render_core.h` — detail field formatters

**Files:**
- Modify: `src/render_core.h`
- Test: `test/test_core/test_main.cpp`

- [ ] **Step 1: Write the failing tests**

Add before `void setUp` (reuses the existing `mkAc` helper):

```cpp
void test_fmtDist(void) {
    TEST_ASSERT_EQUAL_STRING("6 km",   fmtDist(6.0).c_str());
    TEST_ASSERT_EQUAL_STRING("0 km",   fmtDist(-5.0).c_str());   // clamp low
    TEST_ASSERT_EQUAL_STRING("999 km", fmtDist(1500.0).c_str()); // clamp high
}

void test_fmtAlt(void) {
    Aircraft air = mkAc("X", "A320", 35000.0, false, 100.0, 5.0);
    TEST_ASSERT_EQUAL_STRING("10668m", fmtAlt(air).c_str()); // 35000ft -> 10668m
    Aircraft gnd = mkAc("X", "A320", NAN, true, 0.0, 5.0);
    TEST_ASSERT_EQUAL_STRING("GND", fmtAlt(gnd).c_str());
    Aircraft unk = mkAc("X", "A320", NAN, false, 0.0, 5.0);
    TEST_ASSERT_EQUAL_STRING("---", fmtAlt(unk).c_str());
}

void test_fmtSpeed(void) {
    Aircraft air = mkAc("X", "A320", 35000.0, false, 453.6, 5.0);
    TEST_ASSERT_EQUAL_STRING("840", fmtSpeed(air).c_str()); // 453.6kt -> 840km/h
    Aircraft unk = mkAc("X", "A320", 35000.0, false, NAN, 5.0);
    TEST_ASSERT_EQUAL_STRING("---", fmtSpeed(unk).c_str());
}
```

Register in `main()`:

```cpp
    RUN_TEST(test_fmtDist);
    RUN_TEST(test_fmtAlt);
    RUN_TEST(test_fmtSpeed);
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `/opt/homebrew/bin/pio test -e native -f test_core`
Expected: FAIL — `fmtDist`/`fmtAlt`/`fmtSpeed` undefined.

- [ ] **Step 3: Add the formatters to `src/render_core.h`**

```cpp
// Short, non-padded display strings for the detail card.
inline std::string fmtDist(double distKm) {
    long km = std::lround(distKm);
    if (km < 0) km = 0;
    if (km > 999) km = 999;
    char b[16];
    std::snprintf(b, sizeof(b), "%ld km", km);
    return b;
}

inline std::string fmtAlt(const Aircraft& ac) {
    if (ac.onGround) return "GND";
    if (std::isnan(ac.altFt)) return "---";
    long m = std::lround(ftToM(ac.altFt));
    if (m > 99999) m = 99999;
    if (m < -9999) m = -9999;
    char b[12];
    std::snprintf(b, sizeof(b), "%ldm", m);
    return b;
}

inline std::string fmtSpeed(const Aircraft& ac) {
    if (std::isnan(ac.gsKt)) return "---";
    long s = std::lround(ktToKmh(ac.gsKt));
    if (s < 0) s = 0;
    if (s > 9999) s = 9999;
    char b[12];
    std::snprintf(b, sizeof(b), "%ld", s);
    return b;
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `/opt/homebrew/bin/pio test -e native -f test_core`
Expected: PASS (27 tests green).

- [ ] **Step 5: Commit**

```bash
git add src/render_core.h test/test_core/test_main.cpp
git commit -m "feat: add detail field formatters with tests"
```

---

## Task 5: Config + PlatformIO env for the new board

**Files:**
- Modify: `src/config.example.h`
- Modify: `src/config.h` (gitignored — live secrets; edit in place, do not commit)
- Modify: `platformio.ini`

- [ ] **Step 1: Rewrite `src/config.example.h`**

```cpp
#pragma once
// Copy this file to src/config.h and fill in real values.
// config.h is gitignored — secrets never reach the repo.

// --- Wi-Fi (2.4 GHz only; ESP32 has no 5 GHz radio) ---
#define WIFI_SSID   "YourNetwork"
#define WIFI_PASS   "YourPassword"

// --- Observer location (decimal degrees) ---
#define MY_LAT      48.1351
#define MY_LON      11.5820

// --- Search + behavior tunables ---
#define RADIUS_NM         30      // search radius, nautical miles (<=250)
#define POLL_INTERVAL_MS  15000   // API poll period (rate limit is 1 req/s)
#define MAX_AIRCRAFT      10      // how many nearest to show on radar / page through
#define IDLE_RETURN_MS    15000   // detail view auto-returns to radar after this idle
#define SWEEP_PERIOD_MS   4000    // radar sweep: ms per full revolution

// --- Touch CST816S (I2C) on ESP32-S3-Touch-LCD-1.28 ---
#define TOUCH_SDA  6
#define TOUCH_SCL  7
#define TOUCH_INT  5
#define TOUCH_RST  13
// (GC9A01 LCD pins are configured in platformio.ini via TFT_eSPI build flags.)
```

- [ ] **Step 2: Update the live `src/config.h`**

Keep the real WIFI_SSID/WIFI_PASS/MY_LAT/MY_LON already there. Remove the `LCD_*` block and `CYCLE_INTERVAL_MS`. Ensure these exist (add/bump as needed): `MAX_AIRCRAFT 10`, `IDLE_RETURN_MS 15000`, `SWEEP_PERIOD_MS 4000`, and the four `TOUCH_*` defines from Step 1.

- [ ] **Step 3: Rewrite `platformio.ini`**

```ini
[env:esp32-s3]
platform = espressif32
board = esp32-s3-devkitc-1
framework = arduino
monitor_speed = 115200
board_build.arduino.memory_type = qio_qspi
build_flags =
    -std=gnu++17
    -DBOARD_HAS_PSRAM
    -DARDUINO_USB_MODE=1
    -DARDUINO_USB_CDC_ON_BOOT=1
    ; --- TFT_eSPI setup for GC9A01 on ESP32-S3-Touch-LCD-1.28 ---
    -DUSER_SETUP_LOADED=1
    -DGC9A01_DRIVER=1
    -DTFT_WIDTH=240
    -DTFT_HEIGHT=240
    -DTFT_MISO=-1
    -DTFT_MOSI=11
    -DTFT_SCLK=10
    -DTFT_CS=9
    -DTFT_DC=8
    -DTFT_RST=14
    -DTFT_BL=2
    -DTFT_BACKLIGHT_ON=HIGH
    -DLOAD_GLCD=1
    -DLOAD_FONT2=1
    -DLOAD_FONT4=1
    -DSPI_FREQUENCY=40000000
lib_deps =
    bblanchon/ArduinoJson@^7.0.0
    bodmer/TFT_eSPI@^2.5.43

[env:native]
platform = native
test_framework = unity
build_flags = -std=c++17
lib_deps =
    bblanchon/ArduinoJson@^7.0.0
```

- [ ] **Step 4: Verify the native test env still builds and passes**

Run: `/opt/homebrew/bin/pio test -e native -f test_core`
Expected: PASS (27 tests green — the env rename did not touch `native`).

- [ ] **Step 5: Commit**

```bash
git add src/config.example.h platformio.ini
git commit -m "build: switch to esp32-s3 env with TFT_eSPI, update config tunables"
```

---

## Task 6: CST816S touch driver

**Files:**
- Create: `src/cst816s.h`

(No host test — Arduino-only I2C; verified on device in Task 9.)

- [ ] **Step 1: Create `src/cst816s.h`**

```cpp
#pragma once
#include <Arduino.h>
#include <Wire.h>

#define CST816S_ADDR     0x15
#define CST816S_REG_GEST 0x01

// CST816S gesture register values.
enum TouchGesture {
    TG_NONE   = 0x00,
    TG_UP     = 0x01,
    TG_DOWN   = 0x02,
    TG_LEFT   = 0x03,
    TG_RIGHT  = 0x04,
    TG_CLICK  = 0x05,
    TG_DOUBLE = 0x0B,
    TG_LONG   = 0x0C,
};

class CST816S {
public:
    CST816S(int sda, int scl, int rst, int intp)
        : _sda(sda), _scl(scl), _rst(rst), _int(intp) {}

    void begin() {
        pinMode(_rst, OUTPUT);
        digitalWrite(_rst, LOW);  delay(10);
        digitalWrite(_rst, HIGH); delay(50);
        pinMode(_int, INPUT_PULLUP);
        Wire.begin(_sda, _scl);
    }

    // Returns the current gesture register value (TG_* ), or TG_NONE on I2C error.
    uint8_t readGesture() {
        Wire.beginTransmission(CST816S_ADDR);
        Wire.write(CST816S_REG_GEST);
        if (Wire.endTransmission(false) != 0) return TG_NONE;
        if (Wire.requestFrom(CST816S_ADDR, 1) != 1) return TG_NONE;
        return Wire.read();
    }

private:
    int _sda, _scl, _rst, _int;
};
```

- [ ] **Step 2: Commit**

```bash
git add src/cst816s.h
git commit -m "feat: add minimal CST816S touch gesture driver"
```

---

## Task 7: Rewrite `flight_ticker.ino`

**Files:**
- Modify (full rewrite): `src/flight_ticker.ino`

(No host test — Arduino firmware; compiled in Task 8, verified on device in Tasks 9-10.)

- [ ] **Step 1: Replace the entire contents of `src/flight_ticker.ino`**

```cpp
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

    // page-position dots
    int n = (int)g_cache.size();
    int spacing = 12;
    int startX = CX - (n - 1) * spacing / 2;
    for (int i = 0; i < n; i++) {
        uint16_t c = (i == (int)g_idx) ? TFT_CYAN : TFT_DARKGREY;
        fb.fillCircle(startX + i * spacing, 196, 2, c);
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
    if (now - g_lastPoll >= POLL_INTERVAL_MS) { pollApi(); g_lastPoll = now; }

    handleTouch();
    if (g_view == DETAIL && now - g_lastTouch >= IDLE_RETURN_MS) g_view = RADAR;

    if (g_view == RADAR) drawRadar(); else drawDetail();
    delay(16); // ~60 fps cap
}
#endif // ARDUINO
```

- [ ] **Step 2: Commit**

```bash
git add src/flight_ticker.ino
git commit -m "feat: radar + detail carousel firmware for ESP32-S3 round display"
```

---

## Task 8: Compile the firmware

**Files:** none (build verification)

- [ ] **Step 1: Build the esp32-s3 env**

Run: `/opt/homebrew/bin/pio run -e esp32-s3`
Expected: SUCCESS — `=========== [SUCCESS] ===========`. TFT_eSPI and ArduinoJson resolve; no undefined references.

- [ ] **Step 2: If it fails on TFT_eSPI font/driver flags**, re-read the error, confirm the `build_flags` in `platformio.ini` match Task 5 Step 3 exactly, then re-run `/opt/homebrew/bin/pio run -e esp32-s3`. Do not edit files inside the TFT_eSPI library — all config is via build flags.

- [ ] **Step 3: Commit** (only if any fix was needed)

```bash
git add platformio.ini
git commit -m "build: fix esp32-s3 compile flags"
```

---

## Task 9: Flash + on-device radar verification

**Files:** none (device verification). Requires the board connected via USB-C.

- [ ] **Step 1: Upload**

Run: `/opt/homebrew/bin/pio run -e esp32-s3 -t upload`
Expected: `[SUCCESS]`. If esptool errors with `IndexError ... get_security_info` (the prior board's trap), hold the **BOOT** button during upload and release after "Writing…".

- [ ] **Step 2: Confirm serial boot**

Read serial at 115200 (headless workaround if `pio device monitor` throws `termios.error`: use the pyserial-via-PlatformIO-python script from `reference_esp32-platformio-traps`). Expected lines: an IP address, then `poll ok: N aircraft`.

- [ ] **Step 3: Visually verify the radar**

Expected on the round screen: 3 green range rings, an "N" at top, a rotating sweep line with a fading trail, a white center dot, and (if traffic) green blips with the nearest one yellow + labelled. Empty sky shows "NO TRAFFIC"; a red dot top-right appears if a poll fails.

- [ ] **Step 4: Note any display issues** (mirrored/rotated image, wrong colors). If the image is mirrored or rotated, adjust `tft.setRotation(0)` in `setup()` to the correct value (0–3) and, if colors are inverted, add `-DTFT_INVERSION_ON=1` (or `_OFF`) to `build_flags`; re-run Tasks 8–9.

  **If serial shows `sprite alloc failed`, or the board reboots/crashes during the first `poll ok` (heap exhausted by the 115 KB 16-bit sprite competing with the TLS handshake):** change `fb.setColorDepth(16)` to `fb.setColorDepth(8)` in `setup()` (halves the framebuffer to ~58 KB; TFT_eSPI auto-converts the 16-bit color constants). Re-run Tasks 8–9.

- [ ] **Step 5: Commit** (only if rotation/inversion fix was needed)

```bash
git add platformio.ini src/flight_ticker.ino
git commit -m "fix: correct GC9A01 rotation/inversion on device"
```

---

## Task 10: On-device touch + detail verification

**Files:** none (device verification)

- [ ] **Step 1: Tap to open detail**

Tap the screen. Expected: switches to the detail card — cyan callsign, type + compass point, large yellow distance, white altitude/speed row, page dots at bottom (first dot cyan).

- [ ] **Step 2: Swipe to page**

Swipe left → next (farther) aircraft, the active page dot advances. Swipe right → previous, wraps around at the ends.

- [ ] **Step 3: Return to radar**

Tap (or swipe down). Expected: back to the radar view. Also confirm auto-return: open detail, wait ~15 s without touching → it returns to radar on its own.

- [ ] **Step 4: Fix swipe orientation if reversed**

If swipe directions feel inverted/rotated (depends on the panel's mounting vs `setRotation`), remap the `TG_LEFT`/`TG_RIGHT`/`TG_DOWN` cases in `handleTouch()` to match the physical gestures, then re-run Tasks 8–9 and re-test.

- [ ] **Step 5: Commit** (only if a remap was needed)

```bash
git add src/flight_ticker.ino
git commit -m "fix: align touch gesture mapping to physical orientation"
```

---

## Done criteria

- `pio test -e native -f test_core` → 27 tests pass.
- `pio run -e esp32-s3` → builds clean.
- On device: radar animates with correctly-placed blips; tap opens detail; swipes page through nearest aircraft with wrap; tap/swipe-down/15 s-idle return to radar; stale marker on poll failure.
