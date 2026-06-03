# WiFiManager Captive Portal Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace compile-time Wi-Fi credentials with a tzapu/WiFiManager captive portal that also configures the observer location, persisted to NVS, reopenable by a long-press.

**Architecture:** `connectWifi()` uses `WiFiManager.autoConnect("FlightRadar-Setup")` — saved/seed credentials connect directly, otherwise a captive portal (AP + web form) provisions Wi-Fi + observer lat/lon. Location lives in runtime globals `g_obsLat`/`g_obsLon` (replacing the `MY_LAT`/`MY_LON` defines at every use site), loaded from NVS at boot and written by the portal's save callback. A `TG_LONG` touch reopens the portal on demand. A pure `parseLatLon` helper (host-tested) validates portal input; everything else is on-device glue.

**Tech Stack:** C++ (Arduino/ESP32-S3), tzapu/WiFiManager, ESP32 `Preferences` (NVS), TFT_eSPI, PlatformIO native Unity tests.

**Spec:** `docs/superpowers/specs/2026-06-03-wifi-captive-portal-design.md`

---

## File structure

- `src/coord_core.h` (new) — Arduino-free `parseLatLon` (host-tested). **Task 1.**
- `test/test_core/test_main.cpp` — `parseLatLon` tests. **Task 1.**
- `platformio.ini` — add `tzapu/WiFiManager` to the `esp32-s3` env. **Task 2.**
- `src/flight_ticker.ino` — `g_obsLat`/`g_obsLon` globals + `saveLocation` + NVS load + replace `MY_LAT`/`MY_LON` (**Task 2**); `connectWifi` WiFiManager rewrite + `drawSetupScreen` + `setupPortalParams` (**Task 3**); `TG_LONG` on-demand portal (**Task 4**).
- `src/config.example.h` — note creds are now a seed. **Task 5.**

Task 1 is pure TDD. Tasks 2-4 are Arduino glue verified by `pio run -e esp32-s3`. `pio` is `/opt/homebrew/bin/pio`.

---

### Task 1: parseLatLon pure helper (coord_core.h)

**Files:** Create `src/coord_core.h`; Test `test/test_core/test_main.cpp`.

- [ ] **Step 1: Write failing test**

Add this test in `test/test_core/test_main.cpp` immediately before `void setUp(void) {}`:

```cpp
void test_parse_lat_lon(void) {
    double la = 0, lo = 0;
    TEST_ASSERT_TRUE(parseLatLon("38.7677", "-9.3006", la, lo));
    TEST_ASSERT_FLOAT_WITHIN(0.0001, 38.7677, la);
    TEST_ASSERT_FLOAT_WITHIN(0.0001, -9.3006, lo);
    // boundaries accepted
    TEST_ASSERT_TRUE(parseLatLon("-90", "180", la, lo));
    TEST_ASSERT_TRUE(parseLatLon("90", "-180", la, lo));
    // out of range rejected
    double a = 1.5, b = 2.5;
    TEST_ASSERT_FALSE(parseLatLon("91", "0", a, b));
    TEST_ASSERT_FALSE(parseLatLon("0", "181", a, b));
    // garbage / empty / trailing junk rejected, out-params untouched
    TEST_ASSERT_FALSE(parseLatLon("abc", "0", a, b));
    TEST_ASSERT_FALSE(parseLatLon("", "0", a, b));
    TEST_ASSERT_FALSE(parseLatLon("38.7x", "0", a, b));
    TEST_ASSERT_FLOAT_WITHIN(0.0001, 1.5, a);
    TEST_ASSERT_FLOAT_WITHIN(0.0001, 2.5, b);
}
```

Register it in `main()` after `RUN_TEST(test_query_radius_nm);`:

```cpp
    RUN_TEST(test_parse_lat_lon);
```

Add the include near the top of the test file, after the existing `#include "../../src/render_core.h"`:

```cpp
#include "../../src/coord_core.h"
```

- [ ] **Step 2: Run test to verify it fails**

Run: `pio test -e native -f test_core`
Expected: FAIL to compile — `parseLatLon` / `coord_core.h` not found.

- [ ] **Step 3: Implement**

Create `src/coord_core.h`:

```cpp
#pragma once
#include <cstdlib>

// Parse two coordinate strings; on success write lat/lon and return true. Returns
// false (leaving lat/lon untouched) when either string is empty, non-numeric, has
// trailing garbage, or is out of range (lat [-90,90], lon [-180,180]).
inline bool parseLatLon(const char* latStr, const char* lonStr, double& lat, double& lon) {
    if (!latStr || !lonStr || latStr[0] == '\0' || lonStr[0] == '\0') return false;
    char* latEnd = nullptr;
    char* lonEnd = nullptr;
    double la = std::strtod(latStr, &latEnd);
    double lo = std::strtod(lonStr, &lonEnd);
    if (latEnd == latStr || lonEnd == lonStr) return false; // no digits consumed
    if (*latEnd != '\0' || *lonEnd != '\0') return false;   // trailing garbage
    if (la < -90.0 || la > 90.0) return false;
    if (lo < -180.0 || lo > 180.0) return false;
    lat = la;
    lon = lo;
    return true;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `pio test -e native -f test_core`
Expected: PASS — all cases (existing + new `test_parse_lat_lon`).

- [ ] **Step 5: Commit**

```bash
git add src/coord_core.h test/test_core/test_main.cpp
git commit -m "feat(coord): parseLatLon validating coordinate parser + tests"
```

---

### Task 2: WiFiManager dep + observer-location globals + NVS load

**Files:** Modify `platformio.ini`; Modify `src/flight_ticker.ino`.

Arduino glue; verify by `pio run -e esp32-s3`.

- [ ] **Step 1: Add the WiFiManager dependency**

In `platformio.ini`, in the `[env:esp32-s3]` `lib_deps` list, add a line so it reads:

```ini
lib_deps =
    bblanchon/ArduinoJson@^7.0.0
    bodmer/TFT_eSPI@^2.5.43
    h2zero/NimBLE-Arduino@^1.4.1
    tzapu/WiFiManager@^2.0.17
```

(Leave the `[env:native]` `lib_deps` unchanged — `coord_core.h` is Arduino-free.)

- [ ] **Step 2: Include coord_core.h**

In `src/flight_ticker.ino`, after `#include "render_core.h"`, add:

```cpp
#include "coord_core.h"
```

- [ ] **Step 3: Add observer-location globals**

In `src/flight_ticker.ino`, after `int g_rangeIdx = 1;`, add:

```cpp
double g_obsLat = MY_LAT;  // observer location; default from config.h, restored from NVS in setup()
double g_obsLon = MY_LON;
```

- [ ] **Step 4: Add the saveLocation helper**

In `src/flight_ticker.ino`, immediately after the `saveRangeIdx()` function, add:

```cpp
// Persist the observer location to NVS (written by the Wi-Fi setup portal).
void saveLocation(double lat, double lon) {
    Preferences prefs;
    prefs.begin("radar", false);   // read-write
    prefs.putDouble("lat", lat);
    prefs.putDouble("lon", lon);
    prefs.end();
}
```

- [ ] **Step 5: Use the globals in pollApi**

In `src/flight_ticker.ino` `pollApi()`, replace the three `MY_LAT`/`MY_LON` uses:
- URL args `(double)MY_LAT, (double)MY_LON` → `g_obsLat, g_obsLon`
- `parseNearest(std::string(payload.c_str()), MY_LAT, MY_LON, RADAR_PLOT_CAP, HIDE_GROUND_AIRCRAFT)` → `parseNearest(std::string(payload.c_str()), g_obsLat, g_obsLon, RADAR_PLOT_CAP, HIDE_GROUND_AIRCRAFT)`
- `g_centerLat = MY_LAT; g_centerLon = MY_LON;` → `g_centerLat = g_obsLat; g_centerLon = g_obsLon;`

- [ ] **Step 6: Load the location from NVS in setup()**

In `src/flight_ticker.ino` `setup()`, extend the existing `Preferences` block (the one that reads `rangeIdx`) to also read lat/lon and seed the radar center. Replace that block with:

```cpp
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
```

- [ ] **Step 7: Compile**

Run: `pio run -e esp32-s3`
Expected: SUCCESS (WiFiManager downloads + links; `connectWifi` still old but compiles).

- [ ] **Step 8: Commit**

```bash
git add platformio.ini src/flight_ticker.ino
git commit -m "feat(wifi): observer-location globals + NVS load + WiFiManager dep"
```

---

### Task 3: connectWifi WiFiManager rewrite + setup screen

**Files:** Modify `src/flight_ticker.ino`.

Arduino glue; verify by `pio run -e esp32-s3`.

- [ ] **Step 1: Include WiFiManager**

In `src/flight_ticker.ino`, after `#include <WiFi.h>` (top of the file), add:

```cpp
#include <WiFiManager.h>
```

- [ ] **Step 2: Add the setup screen + portal-params helper**

In `src/flight_ticker.ino`, immediately above the existing `void connectWifi()` function, add:

```cpp
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
```

- [ ] **Step 3: Rewrite connectWifi**

In `src/flight_ticker.ino`, replace the entire existing `connectWifi()` function with:

```cpp
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
```

- [ ] **Step 4: Compile**

Run: `pio run -e esp32-s3`
Expected: SUCCESS.

- [ ] **Step 5: Commit**

```bash
git add src/flight_ticker.ino
git commit -m "feat(wifi): captive-portal provisioning via WiFiManager + setup screen"
```

---

### Task 4: TG_LONG on-demand portal

**Files:** Modify `src/flight_ticker.ino`.

Arduino glue; verify by `pio run -e esp32-s3`.

- [ ] **Step 1: Add the on-demand portal helper**

In `src/flight_ticker.ino`, immediately after the `connectWifi()` function, add:

```cpp
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
```

- [ ] **Step 2: Trigger it on TG_LONG in the RADAR branch**

In `src/flight_ticker.ino` `handleTouch()`, the `g_view == RADAR` branch currently handles `TG_CLICK`, `TG_UP`, `TG_DOWN`. Add a `TG_LONG` case. Replace the RADAR branch's closing of the `TG_DOWN` block so the chain reads:

```cpp
        } else if (g == TG_DOWN) {              // zoom out (larger range)
            int n = clampRangeIndex(g_rangeIdx, +1, kRangeCount);
            if (n != g_rangeIdx) { g_rangeIdx = n; saveRangeIdx(); }
        } else if (g == TG_LONG) {              // long-press: reopen Wi-Fi setup portal
            startPortalOnDemand();
        }
```

- [ ] **Step 3: Compile**

Run: `pio run -e esp32-s3`
Expected: SUCCESS.

- [ ] **Step 4: Commit**

```bash
git add src/flight_ticker.ino
git commit -m "feat(wifi): long-press reopens the Wi-Fi setup portal on demand"
```

---

### Task 5: config note + full verify + on-device

**Files:** Modify `src/config.example.h`; then verify.

- [ ] **Step 1: Note the seed semantics in config.example.h**

In `src/config.example.h`, replace the Wi-Fi comment block so it reads:

```cpp
// --- Wi-Fi (2.4 GHz only; ESP32 has no 5 GHz radio) ---
// These are a SEED only: used on a fresh device with empty NVS. Normally you
// provision Wi-Fi (and the observer location) via the "FlightRadar-Setup" captive
// portal — no re-flash needed. Leave as placeholders to always start in the portal.
#define WIFI_SSID   "YourNetwork"
#define WIFI_PASS   "YourPassword"
```

And update the observer-location comment to note it is a default:

```cpp
// --- Observer location (decimal degrees; default until set via the setup portal) ---
#define MY_LAT      48.1351
#define MY_LON      11.5820
```

- [ ] **Step 2: Full native suite**

Run: `pio test -e native -f test_core`
Expected: PASS — all cases incl. `test_parse_lat_lon`.

- [ ] **Step 3: Build firmware**

Run: `pio run -e esp32-s3`
Expected: SUCCESS.

- [ ] **Step 4: Flash + on-device acceptance** (manual, requires hardware)

Run: `pio run -e esp32-s3 -t upload`

Acceptance checklist:
- A device whose `config.h` holds a working network + empty NVS connects via the
  seed (no portal), and the radar centers on the config coordinates.
- Erase/!connectable network → the `FlightRadar-Setup` AP + the LCD setup screen
  appear; joining the AP, picking a network, and entering lat/lon connects and the
  radar centers on the entered coordinates (persisted across reboot).
- Long-press on the radar reopens the portal; changing the network reconnects with
  no re-flash.
- Leaving the portal unconfigured for 180 s boots to the radar offline (NO LINK /
  BLE fallback), no crash.

---

## After implementation

Use superpowers:finishing-a-development-branch to verify tests, then merge + push per the established cadence. With this sub-project the 4-10 feature batch is complete; the remaining note-only follow-up is the accumulated docs drift (README/ARCHITECTURE/HARDWARE/CLAUDE.md still describe the v2 wire, single `RADIUS_NM`, and don't mention detail enrichment, range presets/rim dots, the phone viewer/push, or this captive portal) — a future docs-refresh pass.
