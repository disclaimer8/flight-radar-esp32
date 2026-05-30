# Flight Ticker Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build ESP32 firmware that polls airplanes.live for nearby aircraft and rotates the N nearest across a 16×2 LCD, with all parse/distance/format logic isolated in a host-testable pure core.

**Architecture:** Thin `flight_ticker.ino` (Wi-Fi, HTTP, LCD, `millis()` timers) sits on top of a pure, Arduino-free `flight_core.h` (JSON parse → haversine distance → sort → 16-char formatting). The core compiles and unit-tests on the Mac via a PlatformIO `native` env before any hardware flash. Secrets live in a gitignored `config.h`.

**Tech Stack:** PlatformIO (`esp32dev` + `native`), Arduino framework, ArduinoJson v7, LiquidCrystal_I2C, Unity test framework.

---

## File Structure

- `platformio.ini` — two envs: `esp32dev` (firmware) and `native` (tests).
- `.gitignore` — excludes `src/config.h`, `.pio/`.
- `CLAUDE.md` — the project brief, verbatim, as repo context.
- `README.md` — quick start + troubleshooting ("грабли").
- `src/flight_core.h` — PURE logic, header-only, no Arduino deps. Owns: `Aircraft` struct, `ftToM`, `ktToKmh`, `haversineKm`, `parseNearest`, `formatLine1`, `formatLine2`.
- `src/flight_ticker.ino` — hardware layer. Owns: Wi-Fi connect/reconnect, HTTP GET, two `millis()` timers, LCD driver, boot I2C scan. Wrapped in `#if defined(ARDUINO)` so the native env ignores it.
- `src/config.example.h` — committed template of all secrets/tunables.
- `src/config.h` — gitignored real values (created by the user during setup).
- `test/test_core/test_main.cpp` — Unity tests for the pure core.

---

## Task 1: Project scaffold

**Files:**
- Create: `platformio.ini`
- Create: `.gitignore`
- Create: `src/config.example.h`
- Create: `CLAUDE.md`

- [ ] **Step 1: Create `.gitignore`**

```gitignore
.pio/
src/config.h
```

- [ ] **Step 2: Create `platformio.ini`**

```ini
[env:esp32dev]
platform = espressif32
board = esp32dev
framework = arduino
monitor_speed = 115200
lib_deps =
    bblanchon/ArduinoJson@^7.0.0
    marcoschwartz/LiquidCrystal_I2C@^1.1.4
    ; Parallel (non-PCF8574) mode: set USE_I2C_LCD 0 in config.h and use:
    ; arduino-libraries/LiquidCrystal@^1.0.7

[env:native]
platform = native
test_framework = unity
build_flags = -std=c++17
lib_deps =
    bblanchon/ArduinoJson@^7.0.0
```

- [ ] **Step 3: Create `src/config.example.h`**

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
#define CYCLE_INTERVAL_MS 5000    // per-aircraft screen time
#define MAX_AIRCRAFT      5       // how many nearest to rotate through

// --- LCD (PCF8574 I2C backpack) ---
#define LCD_ADDR    0x27          // try 0x3F if 0x27 shows nothing
#define LCD_SDA     21
#define LCD_SCL     22
```

- [ ] **Step 4: Create `CLAUDE.md`**

Paste the full project brief (the "Flight Ticker — ESP32 + LCD 1602" document) verbatim into `CLAUDE.md`.

- [ ] **Step 5: Commit**

```bash
git add platformio.ini .gitignore src/config.example.h CLAUDE.md
git commit -m "chore: scaffold flight-ticker PlatformIO project"
```

---

## Task 2: Unit conversions (`ftToM`, `ktToKmh`)

**Files:**
- Create: `src/flight_core.h`
- Test: `test/test_core/test_main.cpp`

- [ ] **Step 1: Write the failing test**

Create `test/test_core/test_main.cpp`:

```cpp
#include <unity.h>
#include "../../src/flight_core.h"

void test_ftToM(void) {
    TEST_ASSERT_FLOAT_WITHIN(0.5, 10668.0, ftToM(35000.0)); // 35000 ft ≈ 10668 m
    TEST_ASSERT_EQUAL_FLOAT(0.0, ftToM(0.0));
}

void test_ktToKmh(void) {
    TEST_ASSERT_FLOAT_WITHIN(0.1, 1.852, ktToKmh(1.0));
    TEST_ASSERT_FLOAT_WITHIN(1.0, 840.0, ktToKmh(453.6)); // ~454 kt ≈ 840 km/h
}

void setUp(void) {}
void tearDown(void) {}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_ftToM);
    RUN_TEST(test_ktToKmh);
    return UNITY_END();
}
```

- [ ] **Step 2: Create `src/flight_core.h` with just the conversion helpers**

```cpp
#pragma once
#include <cmath>

inline double ftToM(double ft)  { return ft * 0.3048; }
inline double ktToKmh(double kt) { return kt * 1.852; }
```

- [ ] **Step 3: Run the test to verify it passes**

Run: `pio test -e native -f test_core`
Expected: `test_ftToM` and `test_ktToKmh` PASS.

- [ ] **Step 4: Commit**

```bash
git add src/flight_core.h test/test_core/test_main.cpp
git commit -m "feat: add ft->m and kt->km/h conversions with tests"
```

---

## Task 3: Haversine distance

**Files:**
- Modify: `src/flight_core.h`
- Modify: `test/test_core/test_main.cpp`

- [ ] **Step 1: Add the failing test**

Add to `test_main.cpp` (and register in `main`):

```cpp
void test_haversineKm(void) {
    // 1 degree of longitude at the equator ≈ 111.19 km
    TEST_ASSERT_FLOAT_WITHIN(0.5, 111.19, haversineKm(0.0, 0.0, 0.0, 1.0));
    // Same point => 0
    TEST_ASSERT_FLOAT_WITHIN(0.01, 0.0, haversineKm(48.0, 11.0, 48.0, 11.0));
    // Munich area sanity: ~0.1 deg lat ≈ 11.1 km
    TEST_ASSERT_FLOAT_WITHIN(0.5, 11.12, haversineKm(48.0, 11.0, 48.1, 11.0));
}
```

Add `RUN_TEST(test_haversineKm);` to `main`.

- [ ] **Step 2: Run to verify it fails**

Run: `pio test -e native -f test_core`
Expected: FAIL — `haversineKm` not declared.

- [ ] **Step 3: Implement `haversineKm` in `flight_core.h`**

Add after the conversion helpers:

```cpp
inline double haversineKm(double lat1, double lon1, double lat2, double lon2) {
    const double R = 6371.0; // mean Earth radius, km
    const double toRad = M_PI / 180.0;
    double dLat = (lat2 - lat1) * toRad;
    double dLon = (lon2 - lon1) * toRad;
    double a = std::sin(dLat / 2) * std::sin(dLat / 2) +
               std::cos(lat1 * toRad) * std::cos(lat2 * toRad) *
               std::sin(dLon / 2) * std::sin(dLon / 2);
    return R * 2 * std::atan2(std::sqrt(a), std::sqrt(1 - a));
}
```

- [ ] **Step 4: Run to verify it passes**

Run: `pio test -e native -f test_core`
Expected: all three haversine assertions PASS.

- [ ] **Step 5: Commit**

```bash
git add src/flight_core.h test/test_core/test_main.cpp
git commit -m "feat: add haversine distance with tests"
```

---

## Task 4: `Aircraft` struct + `parseNearest`

**Files:**
- Modify: `src/flight_core.h`
- Modify: `test/test_core/test_main.cpp`

- [ ] **Step 1: Add the failing test**

Add to `test_main.cpp`:

```cpp
static const char* SAMPLE_JSON =
  "{\"ac\":["
    "{\"hex\":\"3c6abc\",\"flight\":\"DLH4AB  \",\"t\":\"A320\",\"alt_baro\":35000,\"gs\":453.6,\"lat\":48.10,\"lon\":11.00},"
    "{\"hex\":\"abc123\",\"flight\":\"BAW123  \",\"t\":\"B772\",\"alt_baro\":\"ground\",\"gs\":12.0,\"lat\":48.50,\"lon\":11.00},"
    "{\"hex\":\"def456\",\"flight\":\"RYR9XZ  \",\"t\":\"B738\",\"alt_baro\":12000,\"gs\":380.0,\"lat\":48.30,\"lon\":11.00}"
  "],\"msg\":\"No error\",\"now\":1.0,\"total\":3}";

void test_parseNearest_sorts_and_trims(void) {
    // Observer at 48.0/11.0. By latitude delta the order is:
    // DLH4AB (0.10), RYR9XZ (0.30), BAW123 (0.50).
    auto list = parseNearest(SAMPLE_JSON, 48.0, 11.0, 2);
    TEST_ASSERT_EQUAL_UINT32(2, list.size());          // trimmed to maxN
    TEST_ASSERT_EQUAL_STRING("DLH4AB", list[0].callsign.c_str()); // trimmed, nearest
    TEST_ASSERT_EQUAL_STRING("RYR9XZ", list[1].callsign.c_str());
    TEST_ASSERT_TRUE(list[0].distKm < list[1].distKm);
}

void test_parseNearest_handles_ground_and_fields(void) {
    auto list = parseNearest(SAMPLE_JSON, 48.0, 11.0, 5);
    TEST_ASSERT_EQUAL_UINT32(3, list.size());
    // Find the ground aircraft (BAW123)
    const Aircraft* gnd = nullptr;
    for (auto& a : list) if (a.callsign == "BAW123") gnd = &a;
    TEST_ASSERT_NOT_NULL(gnd);
    TEST_ASSERT_TRUE(gnd->onGround);
    TEST_ASSERT_EQUAL_STRING("B772", gnd->type.c_str());
}

void test_parseNearest_empty(void) {
    auto list = parseNearest("{\"ac\":[],\"total\":0}", 48.0, 11.0, 5);
    TEST_ASSERT_EQUAL_UINT32(0, list.size());
}
```

Register all three with `RUN_TEST` in `main`.

- [ ] **Step 2: Run to verify it fails**

Run: `pio test -e native -f test_core`
Expected: FAIL — `Aircraft` / `parseNearest` not declared.

- [ ] **Step 3: Implement the struct + parser in `flight_core.h`**

Add includes at the top of `flight_core.h` (below `#pragma once`):

```cpp
#include <ArduinoJson.h>
#include <vector>
#include <string>
#include <algorithm>
```

Add after `haversineKm`:

```cpp
struct Aircraft {
    std::string callsign;  // trimmed "flight", "" if absent
    std::string type;      // "t", "" if absent
    double altFt = NAN;    // alt_baro numeric; NAN if missing/ground
    bool   onGround = false;
    double gsKt = NAN;     // ground speed knots; NAN if missing
    double lat = 0.0;
    double lon = 0.0;
    double distKm = 0.0;   // filled by parseNearest
};

inline std::string trimStr(const char* s) {
    if (!s) return "";
    std::string v(s);
    size_t a = v.find_first_not_of(" \t");
    size_t b = v.find_last_not_of(" \t");
    if (a == std::string::npos) return "";
    return v.substr(a, b - a + 1);
}

inline std::vector<Aircraft> parseNearest(const std::string& json,
                                          double myLat, double myLon,
                                          size_t maxN) {
    std::vector<Aircraft> out;

    JsonDocument filter;
    filter["ac"][0]["flight"] = true;
    filter["ac"][0]["t"] = true;
    filter["ac"][0]["alt_baro"] = true;
    filter["ac"][0]["gs"] = true;
    filter["ac"][0]["lat"] = true;
    filter["ac"][0]["lon"] = true;

    JsonDocument doc;
    DeserializationError err =
        deserializeJson(doc, json, DeserializationOption::Filter(filter));
    if (err) return out;

    for (JsonObject a : doc["ac"].as<JsonArray>()) {
        Aircraft ac;
        ac.callsign = trimStr(a["flight"].as<const char*>());
        ac.type     = trimStr(a["t"].as<const char*>());
        if (a["alt_baro"].is<const char*>()) {
            ac.onGround = (std::string(a["alt_baro"].as<const char*>()) == "ground");
        } else if (a["alt_baro"].is<double>()) {
            ac.altFt = a["alt_baro"].as<double>();
        }
        if (a["gs"].is<double>()) ac.gsKt = a["gs"].as<double>();
        ac.lat = a["lat"] | 0.0;
        ac.lon = a["lon"] | 0.0;
        ac.distKm = haversineKm(myLat, myLon, ac.lat, ac.lon);
        out.push_back(ac);
    }

    std::sort(out.begin(), out.end(),
              [](const Aircraft& x, const Aircraft& y) { return x.distKm < y.distKm; });
    if (out.size() > maxN) out.resize(maxN);
    return out;
}
```

- [ ] **Step 4: Run to verify it passes**

Run: `pio test -e native -f test_core`
Expected: all parse tests PASS.

- [ ] **Step 5: Commit**

```bash
git add src/flight_core.h test/test_core/test_main.cpp
git commit -m "feat: parse airplanes.live response into sorted nearest list"
```

---

## Task 5: Display formatting (`formatLine1`, `formatLine2`)

**Files:**
- Modify: `src/flight_core.h`
- Modify: `test/test_core/test_main.cpp`

- [ ] **Step 1: Add the failing test**

Add to `test_main.cpp`:

```cpp
static Aircraft mkAc(std::string cs, std::string t, double altFt,
                     bool gnd, double gsKt, double distKm) {
    Aircraft a;
    a.callsign = cs; a.type = t; a.altFt = altFt;
    a.onGround = gnd; a.gsKt = gsKt; a.distKm = distKm;
    return a;
}

void test_formatLine1_basic(void) {
    Aircraft a = mkAc("DLH4AB", "A320", 35000, false, 453.6, 12.0);
    std::string l1 = formatLine1(a);
    TEST_ASSERT_EQUAL_UINT32(16, l1.size());          // always exactly 16
    TEST_ASSERT_EQUAL_STRING("DLH4AB      12km", l1.c_str());
}

void test_formatLine1_empty_callsign(void) {
    Aircraft a = mkAc("", "A320", 35000, false, 453.6, 5.0);
    std::string l1 = formatLine1(a);
    TEST_ASSERT_EQUAL_UINT32(16, l1.size());
    TEST_ASSERT_EQUAL_STRING("------       5km", l1.c_str());
}

void test_formatLine2_basic(void) {
    Aircraft a = mkAc("DLH4AB", "A320", 35000, false, 453.6, 12.0);
    std::string l2 = formatLine2(a);            // 35000ft->10668m, 453.6kt->840km/h
    TEST_ASSERT_EQUAL_UINT32(16, l2.size());
    TEST_ASSERT_EQUAL_STRING("A320 10668m 840 ", l2.c_str());
}

void test_formatLine2_ground(void) {
    Aircraft a = mkAc("BAW123", "B772", NAN, true, 12.0, 30.0);
    std::string l2 = formatLine2(a);
    TEST_ASSERT_EQUAL_UINT32(16, l2.size());
    TEST_ASSERT_EQUAL_STRING("B772 GND 22     ", l2.c_str()); // 12kt->22km/h
}
```

Register all four with `RUN_TEST` in `main`.

- [ ] **Step 2: Run to verify it fails**

Run: `pio test -e native -f test_core`
Expected: FAIL — `formatLine1` / `formatLine2` not declared.

- [ ] **Step 3: Implement the formatters in `flight_core.h`**

Add `#include <cstdio>` to the includes, then add at the end of the file:

```cpp
inline std::string padTo16(std::string s) {
    if (s.size() > 16) return s.substr(0, 16);
    s.append(16 - s.size(), ' ');
    return s;
}

inline std::string formatLine1(const Aircraft& ac) {
    std::string left = ac.callsign.empty() ? std::string("------")
                                           : ac.callsign;
    if (left.size() > 8) left = left.substr(0, 8);

    char dist[8];
    long km = std::lround(ac.distKm);
    if (km > 999) km = 999;
    std::snprintf(dist, sizeof(dist), "%ldkm", km);
    std::string right(dist);

    int pad = 16 - (int)left.size() - (int)right.size();
    if (pad < 1) { left = left.substr(0, 16 - right.size() - 1); pad = 1; }
    return left + std::string(pad, ' ') + right;
}

inline std::string formatLine2(const Aircraft& ac) {
    std::string type = ac.type.empty() ? std::string("----") : ac.type;
    if (type.size() > 4) type = type.substr(0, 4);

    std::string altStr;
    if (ac.onGround) altStr = "GND";
    else if (std::isnan(ac.altFt)) altStr = "---";
    else { char b[8]; std::snprintf(b, sizeof(b), "%ldm", std::lround(ftToM(ac.altFt))); altStr = b; }

    std::string spdStr;
    if (std::isnan(ac.gsKt)) spdStr = "---";
    else { char b[8]; std::snprintf(b, sizeof(b), "%ld", std::lround(ktToKmh(ac.gsKt))); spdStr = b; }

    return padTo16(type + " " + altStr + " " + spdStr);
}
```

- [ ] **Step 4: Run to verify it passes**

Run: `pio test -e native -f test_core`
Expected: all formatting tests PASS, plus all earlier tests still green.

- [ ] **Step 5: Commit**

```bash
git add src/flight_core.h test/test_core/test_main.cpp
git commit -m "feat: add 16-char LCD line formatters with tests"
```

---

## Task 6: Firmware layer (`flight_ticker.ino`)

This task is hardware code, verified by compilation (`pio run`), not unit tests.

**Files:**
- Create: `src/flight_ticker.ino`

- [ ] **Step 1: Write `src/flight_ticker.ino`**

```cpp
#if defined(ARDUINO)
#include <Arduino.h>
#include <WiFi.h>
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

    char url[128];
    std::snprintf(url, sizeof(url),
        "http://api.airplanes.live/v2/point/%.4f/%.4f/%d",
        (double)MY_LAT, (double)MY_LON, (int)RADIUS_NM);

    HTTPClient http;
    http.begin(url);
    http.setUserAgent("flight-ticker-esp32");
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
    std::string l1 = formatLine1(ac);
    if (g_stale) l1[15] = '*';   // stale indicator in the last column
    lcdShow(l1, formatLine2(ac));
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
```

- [ ] **Step 2: Create a real `config.h` so the firmware compiles**

```bash
cp src/config.example.h src/config.h
```

(Real Wi-Fi/coords get filled in during the setup conversation; placeholder values compile fine.)

- [ ] **Step 3: Compile the firmware**

Run: `pio run -e esp32dev`
Expected: `SUCCESS`. If `LiquidCrystal_I2C` fails to resolve, run `pio pkg search LiquidCrystal_I2C` and substitute a working id in `platformio.ini`, then re-run.

- [ ] **Step 4: Re-run native tests to confirm the core still builds standalone**

Run: `pio test -e native -f test_core`
Expected: all tests PASS (the `#if defined(ARDUINO)` guard keeps the `.ino` out of the native build).

- [ ] **Step 5: Commit**

```bash
git add src/flight_ticker.ino
git commit -m "feat: add ESP32 firmware layer (WiFi/HTTP/LCD/timers)"
```

---

## Task 7: README

**Files:**
- Create: `README.md`

- [ ] **Step 1: Write `README.md`**

```markdown
# Flight Ticker

ESP32 + 1602 LCD that shows the nearest aircraft from airplanes.live.

## Setup

1. Install PlatformIO Core: `brew install platformio`
2. `cp src/config.example.h src/config.h` and fill in Wi-Fi (2.4 GHz only),
   your lat/lon, and `RADIUS_NM`.
3. Run the host tests: `pio test -e native -f test_core`
4. Build: `pio run -e esp32dev`
5. Find the port: `ls /dev/cu.*` (CH340 → `cu.usbserial-*`, CP2102 → `cu.SLAB_USBtoUART`).
   No port? Install the CP210x or CH34x driver and reconnect USB.
6. Flash: `pio run -e esp32dev -t upload`. If upload stalls, hold **BOOT** as it
   starts "Connecting...".
7. Monitor: `pio device monitor -b 115200`.

## Wiring (I2C, PCF8574 backpack)

VCC→3V3, GND→GND, SDA→GPIO21, SCL→GPIO22.

## Troubleshooting (грабли)

- **Backlight on, screen blank** → contrast. Turn the trimmer on the I2C backpack.
- **Garbage / wrong I2C address** → boot serial prints an I2C scan. Set `LCD_ADDR`
  in `config.h` to the found address (`0x27` or `0x3F`).
- **Port not visible** → missing CP210x/CH34x driver, or a charge-only USB cable.
- **`No aircraft`** → normal when the sky is empty; raise `RADIUS_NM` to test.
- **API limit** → don't poll faster than 1 req/s (firmware uses 15 s).
```

- [ ] **Step 2: Commit**

```bash
git add README.md
git commit -m "docs: add README with setup and troubleshooting"
```

---

## Self-Review Notes

- **Spec coverage:** thin-core architecture (Task 6 + core tasks), non-blocking dual-timer loop (Task 6), API endpoint + UA + http (Task 6), haversine/sort/top-N (Tasks 3–4), km/m units + 16-char layout (Task 5), empty/Wi-Fi/HTTP-error/stale + boot I2C scan (Task 6), config.h gitignored + example (Task 1), native tests (Tasks 2–5), README troubleshooting (Task 7). All spec sections mapped.
- **Type consistency:** `Aircraft` fields (`callsign/type/altFt/onGround/gsKt/lat/lon/distKm`), `parseNearest`, `formatLine1/2`, `ftToM`, `ktToKmh`, `haversineKm` used identically across tasks 2–6.
- **Naming caveat for executor:** `.ino` is wrapped in `#if defined(ARDUINO)` so `pio test -e native` ignores it; do not remove that guard.
