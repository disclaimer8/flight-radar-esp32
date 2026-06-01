# BLE Fallback (firmware) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a BLE fallback data path so a phone companion can feed nearby-aircraft data to the device over Bluetooth when Wi-Fi is unavailable, centered on the phone's GPS.

**Architecture:** A new pure, host-tested wire parser (`ble_core.h`) decodes a compact binary packet into the existing `Aircraft` type. The firmware gains a NimBLE peripheral with one ingest characteristic, a Wi-Fi/BLE source-arbitration state, and a variable radar center. The phone (sub-project B) is out of scope; a Python `bleak` script is the interim sender for on-device verification.

**Tech Stack:** PlatformIO (ESP32-S3 Arduino + native Unity), NimBLE-Arduino, TFT_eSPI, ArduinoJson, Python `bleak` (test harness).

---

## File Structure

- `src/ble_core.h` — **new.** Pure, Arduino-free parser: wire-format constants + `parseBlePacket()` → `BlePacket` (center + decoded, distance-filled, sorted, capped `Aircraft`). Host-tested.
- `test/test_core/test_main.cpp` — **modify.** Add `parseBlePacket` tests (existing 29 stay).
- `platformio.ini` — **modify.** Add NimBLE dep to the `esp32-s3` env (native env unchanged).
- `src/config.example.h` / `src/config.h` — **modify.** Add `BLE_FRESHNESS_MS`.
- `src/flight_ticker.ino` — **modify.** Variable radar center, non-blocking Wi-Fi poll, source arbitration + on-screen indicator, NimBLE peripheral + packet handling.
- `scripts/ble_send.py` — **new.** Python `bleak` sender stub for on-device testing.

---

## Task 1: `ble_core.h` — wire parser (pure, host-tested)

**Files:**
- Create: `src/ble_core.h`
- Test: `test/test_core/test_main.cpp`

- [ ] **Step 1: Write the failing tests**

At the top of `test/test_core/test_main.cpp`, after the existing includes (after line 3 `#include "../../src/render_core.h"`), add:

```cpp
#include "../../src/ble_core.h"
#include <cstring>
```

Add these helpers and tests before `void setUp`:

```cpp
// --- BLE packet test helpers ---
static void blePutF32(std::vector<uint8_t>& v, float f) {
    uint8_t b[4]; std::memcpy(b, &f, 4); v.insert(v.end(), b, b + 4);
}
static void blePutI32(std::vector<uint8_t>& v, int32_t x) {
    uint8_t b[4]; std::memcpy(b, &x, 4); v.insert(v.end(), b, b + 4);
}
static void blePutI16(std::vector<uint8_t>& v, int16_t x) {
    uint8_t b[2]; std::memcpy(b, &x, 2); v.insert(v.end(), b, b + 2);
}
static void blePutField(std::vector<uint8_t>& v, const char* s, size_t n) {
    size_t L = std::strlen(s);
    for (size_t i = 0; i < n; i++) v.push_back(i < L ? (uint8_t)s[i] : (uint8_t)' ');
}
static std::vector<uint8_t> bleHeader(uint8_t count, float clat, float clon) {
    std::vector<uint8_t> v;
    v.push_back(BLE_MAGIC0); v.push_back(BLE_MAGIC1);
    v.push_back(BLE_VERSION); v.push_back(count);
    blePutF32(v, clat); blePutF32(v, clon);
    return v;
}
static void bleAddRecord(std::vector<uint8_t>& v, const char* cs, const char* ty,
                         float lat, float lon, int32_t alt, int16_t gs, uint8_t flags) {
    blePutField(v, cs, 8); blePutField(v, ty, 4);
    blePutF32(v, lat); blePutF32(v, lon);
    blePutI32(v, alt); blePutI16(v, gs);
    v.push_back(flags); v.push_back(0);
}

void test_ble_valid_two_aircraft(void) {
    // Center 48,11. Record A is farther (48.5), B is nearer (48.1).
    std::vector<uint8_t> v = bleHeader(2, 48.0f, 11.0f);
    bleAddRecord(v, "DLH4AB", "A320", 48.5f, 11.0f, 35000,  453, BLE_FLAG_ALT_VALID | BLE_FLAG_GS_VALID);
    bleAddRecord(v, "RYR9XZ", "B738", 48.1f, 11.0f, 12000,  380, BLE_FLAG_ALT_VALID | BLE_FLAG_GS_VALID);
    BlePacket p = parseBlePacket(v.data(), v.size(), 5);
    TEST_ASSERT_TRUE(p.ok);
    TEST_ASSERT_FLOAT_WITHIN(0.001, 48.0, p.centerLat);
    TEST_ASSERT_FLOAT_WITHIN(0.001, 11.0, p.centerLon);
    TEST_ASSERT_EQUAL_UINT32(2, p.aircraft.size());
    TEST_ASSERT_EQUAL_STRING("RYR9XZ", p.aircraft[0].callsign.c_str()); // nearest first
    TEST_ASSERT_EQUAL_STRING("DLH4AB", p.aircraft[1].callsign.c_str());
    TEST_ASSERT_TRUE(p.aircraft[0].distKm < p.aircraft[1].distKm);
    TEST_ASSERT_EQUAL_STRING("B738", p.aircraft[0].type.c_str());
    TEST_ASSERT_FLOAT_WITHIN(0.5, 12000.0, p.aircraft[0].altFt);
    TEST_ASSERT_FLOAT_WITHIN(0.5, 380.0, p.aircraft[0].gsKt);
}

void test_ble_bad_magic(void) {
    std::vector<uint8_t> v = bleHeader(0, 48.0f, 11.0f);
    v[0] = 0x00;
    BlePacket p = parseBlePacket(v.data(), v.size(), 5);
    TEST_ASSERT_FALSE(p.ok);
}

void test_ble_bad_version(void) {
    std::vector<uint8_t> v = bleHeader(0, 48.0f, 11.0f);
    v[2] = 99;
    BlePacket p = parseBlePacket(v.data(), v.size(), 5);
    TEST_ASSERT_FALSE(p.ok);
}

void test_ble_count_overflow(void) {
    std::vector<uint8_t> v = bleHeader(17, 48.0f, 11.0f); // > BLE_MAX_AIRCRAFT
    BlePacket p = parseBlePacket(v.data(), v.size(), 5);
    TEST_ASSERT_FALSE(p.ok);
}

void test_ble_length_mismatch(void) {
    // count says 2 but only one record present
    std::vector<uint8_t> v = bleHeader(2, 48.0f, 11.0f);
    bleAddRecord(v, "AAA", "A320", 48.1f, 11.0f, 10000, 300, BLE_FLAG_ALT_VALID);
    BlePacket p = parseBlePacket(v.data(), v.size(), 5);
    TEST_ASSERT_FALSE(p.ok);
}

void test_ble_flags(void) {
    std::vector<uint8_t> v = bleHeader(1, 0.0f, 0.0f);
    bleAddRecord(v, "GND1", "B772", 0.0f, 0.1f, 0, 5, BLE_FLAG_GROUND); // ground, alt/gs invalid
    BlePacket p = parseBlePacket(v.data(), v.size(), 5);
    TEST_ASSERT_TRUE(p.ok);
    TEST_ASSERT_TRUE(p.aircraft[0].onGround);
    TEST_ASSERT_TRUE(std::isnan(p.aircraft[0].altFt));
    TEST_ASSERT_TRUE(std::isnan(p.aircraft[0].gsKt));
}

void test_ble_caps_to_maxN(void) {
    std::vector<uint8_t> v = bleHeader(3, 0.0f, 0.0f);
    bleAddRecord(v, "C", "A320", 0.0f, 3.0f, 1, 1, BLE_FLAG_ALT_VALID); // farthest
    bleAddRecord(v, "A", "A320", 0.0f, 1.0f, 1, 1, BLE_FLAG_ALT_VALID); // nearest
    bleAddRecord(v, "B", "A320", 0.0f, 2.0f, 1, 1, BLE_FLAG_ALT_VALID);
    BlePacket p = parseBlePacket(v.data(), v.size(), 2);
    TEST_ASSERT_TRUE(p.ok);
    TEST_ASSERT_EQUAL_UINT32(2, p.aircraft.size());        // capped
    TEST_ASSERT_EQUAL_STRING("A", p.aircraft[0].callsign.c_str()); // two nearest kept
    TEST_ASSERT_EQUAL_STRING("B", p.aircraft[1].callsign.c_str());
}
```

Register in `main()` after the last `RUN_TEST`:

```cpp
    RUN_TEST(test_ble_valid_two_aircraft);
    RUN_TEST(test_ble_bad_magic);
    RUN_TEST(test_ble_bad_version);
    RUN_TEST(test_ble_count_overflow);
    RUN_TEST(test_ble_length_mismatch);
    RUN_TEST(test_ble_flags);
    RUN_TEST(test_ble_caps_to_maxN);
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `/opt/homebrew/bin/pio test -e native -f test_core`
Expected: FAIL — `ble_core.h` not found / `parseBlePacket` undefined.

- [ ] **Step 3: Create `src/ble_core.h`**

```cpp
#pragma once
#include <cstdint>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>
#include "flight_core.h"   // Aircraft, haversineKm

// Compact BLE wire protocol (little-endian; both ESP32 and host are LE).
constexpr uint8_t BLE_MAGIC0       = 0x46; // 'F'
constexpr uint8_t BLE_MAGIC1       = 0x52; // 'R'
constexpr uint8_t BLE_VERSION      = 1;
constexpr size_t  BLE_MAX_AIRCRAFT = 16;
constexpr size_t  BLE_HEADER_SIZE  = 12;
constexpr size_t  BLE_RECORD_SIZE  = 28;
constexpr size_t  BLE_MAX_PACKET   = BLE_HEADER_SIZE + BLE_MAX_AIRCRAFT * BLE_RECORD_SIZE; // 460

constexpr uint8_t BLE_FLAG_GROUND    = 0x01;
constexpr uint8_t BLE_FLAG_ALT_VALID = 0x02;
constexpr uint8_t BLE_FLAG_GS_VALID  = 0x04;

struct BlePacket {
    bool   ok = false;
    double centerLat = 0.0;
    double centerLon = 0.0;
    std::vector<Aircraft> aircraft;  // distKm filled, sorted nearest-first, capped to maxN
};

// Trim a fixed-width ASCII field (space-padded, possibly NUL-terminated).
inline std::string bleField(const uint8_t* p, size_t n) {
    std::string s(reinterpret_cast<const char*>(p), n);
    size_t z = s.find('\0');
    if (z != std::string::npos) s.resize(z);
    size_t a = s.find_first_not_of(' ');
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(' ');
    return s.substr(a, b - a + 1);
}

// Decode one binary packet. Returns ok=false (empty) on any validation failure.
inline BlePacket parseBlePacket(const uint8_t* buf, size_t len, size_t maxN) {
    BlePacket out;
    if (!buf || len < BLE_HEADER_SIZE) return out;
    if (buf[0] != BLE_MAGIC0 || buf[1] != BLE_MAGIC1) return out;
    if (buf[2] != BLE_VERSION) return out;
    uint8_t count = buf[3];
    if (count > BLE_MAX_AIRCRAFT) return out;
    if (len != BLE_HEADER_SIZE + (size_t)count * BLE_RECORD_SIZE) return out;

    float clat, clon;
    std::memcpy(&clat, buf + 4, 4);
    std::memcpy(&clon, buf + 8, 4);
    out.centerLat = clat;
    out.centerLon = clon;

    for (uint8_t i = 0; i < count; i++) {
        const uint8_t* r = buf + BLE_HEADER_SIZE + (size_t)i * BLE_RECORD_SIZE;
        Aircraft ac;
        ac.callsign = bleField(r, 8);
        ac.type     = bleField(r + 8, 4);
        float lat, lon; int32_t altFt; int16_t gsKt;
        std::memcpy(&lat,   r + 12, 4);
        std::memcpy(&lon,   r + 16, 4);
        std::memcpy(&altFt, r + 20, 4);
        std::memcpy(&gsKt,  r + 24, 2);
        uint8_t flags = r[26];
        ac.lat = lat;
        ac.lon = lon;
        ac.onGround = (flags & BLE_FLAG_GROUND) != 0;
        ac.altFt = (flags & BLE_FLAG_ALT_VALID) ? (double)altFt : NAN;
        ac.gsKt  = (flags & BLE_FLAG_GS_VALID)  ? (double)gsKt  : NAN;
        ac.distKm = haversineKm(out.centerLat, out.centerLon, ac.lat, ac.lon);
        out.aircraft.push_back(ac);
    }
    std::sort(out.aircraft.begin(), out.aircraft.end(),
              [](const Aircraft& a, const Aircraft& b) { return a.distKm < b.distKm; });
    if (out.aircraft.size() > maxN) out.aircraft.resize(maxN);
    out.ok = true;
    return out;
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `/opt/homebrew/bin/pio test -e native -f test_core`
Expected: PASS — 36 test cases (29 existing + 7 new).

- [ ] **Step 5: Commit**

```bash
git add src/ble_core.h test/test_core/test_main.cpp
git commit -m "feat: add ble_core wire-packet parser with tests"
```

---

## Task 2: NimBLE dependency + config

**Files:**
- Modify: `platformio.ini`
- Modify: `src/config.example.h`
- Modify: `src/config.h` (gitignored — edit in place)

- [ ] **Step 1: Add NimBLE to the esp32-s3 env**

In `platformio.ini`, change the `[env:esp32-s3]` `lib_deps` block from:

```ini
lib_deps =
    bblanchon/ArduinoJson@^7.0.0
    bodmer/TFT_eSPI@^2.5.43
```

to:

```ini
lib_deps =
    bblanchon/ArduinoJson@^7.0.0
    bodmer/TFT_eSPI@^2.5.43
    h2zero/NimBLE-Arduino@^1.4.1
```

Leave the `[env:native]` section unchanged (the parser is pure; native does not need NimBLE).

- [ ] **Step 2: Add `BLE_FRESHNESS_MS` to `src/config.example.h`**

After the `SWEEP_PERIOD_MS` line in the "Search + behavior tunables" block, add:

```cpp
#define BLE_FRESHNESS_MS  30000   // BLE-fed data is considered live for this long
```

- [ ] **Step 3: Add the same define to the live `src/config.h`**

Add `#define BLE_FRESHNESS_MS 30000` to `src/config.h` (preserve all existing real values).

- [ ] **Step 4: Verify the native test env still builds and passes**

Run: `/opt/homebrew/bin/pio test -e native -f test_core`
Expected: PASS — 36 test cases (adding a lib to the esp32-s3 env does not affect `native`).

- [ ] **Step 5: Commit**

```bash
git add platformio.ini src/config.example.h
git commit -m "build: add NimBLE-Arduino dep and BLE_FRESHNESS_MS config"
```

---

## Task 3: Firmware — variable center, non-blocking Wi-Fi, source arbitration + indicator

**Files:**
- Modify: `src/flight_ticker.ino`

(No host test — Arduino firmware; verified by compile in Task 6 and on-device in Task 7. After this task the BLE branch is present but dormant: `g_bleLastRx` stays 0, so `SRC_BLE` never triggers and the Wi-Fi behavior is unchanged.)

- [ ] **Step 1: Add source/center globals**

In `src/flight_ticker.ino`, replace this block:

```cpp
std::vector<Aircraft> g_cache;
unsigned long g_lastPoll  = 0;
unsigned long g_lastTouch = 0;
bool g_stale = false;
```

with:

```cpp
std::vector<Aircraft> g_cache;
unsigned long g_lastPoll  = 0;
unsigned long g_lastTouch = 0;
bool g_stale = false;

// Data source arbitration: Wi-Fi is primary; BLE (phone gateway) is the fallback.
enum Source { SRC_NONE, SRC_WIFI, SRC_BLE };
Source        g_source    = SRC_NONE;
double        g_centerLat = MY_LAT;   // radar center: config in Wi-Fi mode, packet GPS in BLE mode
double        g_centerLon = MY_LON;
unsigned long g_bleLastRx = 0;        // millis of last accepted BLE packet (set in Task 4)
```

- [ ] **Step 2: Make Wi-Fi reconnect non-blocking and stop `pollApi` from blocking on reconnect**

Replace the whole `connectWifi()` function with:

```cpp
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
```

Replace the first line of `pollApi()`:

```cpp
    if (WiFi.status() != WL_CONNECTED) { connectWifi(); if (WiFi.status() != WL_CONNECTED) { g_stale = true; return; } }
```

with (just drop the blocking reconnect — `loop()` only calls `pollApi()` when already connected):

```cpp
    // Called only when WiFi is connected (see loop()). No blocking reconnect here.
```

- [ ] **Step 3: Set the center on a successful Wi-Fi poll**

In `pollApi()`, inside the `if (code == 200) {` block, after `if (g_idx >= g_cache.size()) g_idx = 0;`, add:

```cpp
        g_centerLat = MY_LAT; g_centerLon = MY_LON;
```

- [ ] **Step 4: Use the variable center in the renderers**

In `drawRadar()`, change:

```cpp
        double b = bearingDeg(MY_LAT, MY_LON, ac.lat, ac.lon);
```
to:
```cpp
        double b = bearingDeg(g_centerLat, g_centerLon, ac.lat, ac.lon);
```

In `drawDetail()`, change:

```cpp
    double b = bearingDeg(MY_LAT, MY_LON, ac.lat, ac.lon);
```
to:
```cpp
    double b = bearingDeg(g_centerLat, g_centerLon, ac.lat, ac.lon);
```

- [ ] **Step 5: Replace the stale dot with a visible source indicator**

In `drawRadar()`, replace this block:

```cpp
    if (g_stale) fb.fillCircle(228, 12, 4, TFT_RED);

    fb.pushSprite(0, 0);
}
```

with (the old dot at 228,12 sat in the black corner outside the round panel; this puts a clear W/B/NO-LINK status at the bottom of the visible circle):

```cpp
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
```

- [ ] **Step 6: Guard the poll and arbitrate the source in `loop()`**

Replace the body of `loop()` from the poll line through the idle-return line:

```cpp
    if (now - g_lastPoll >= POLL_INTERVAL_MS) { pollApi(); g_lastPoll = now; }

    handleTouch();
    if (g_view == DETAIL && now - g_lastTouch >= IDLE_RETURN_MS) g_view = RADAR;
```

with:

```cpp
    if (now - g_lastPoll >= POLL_INTERVAL_MS) {
        if (WiFi.status() == WL_CONNECTED) pollApi();   // skip when offline; no blocking reconnect
        g_lastPoll = now;
    }

    // Source arbitration: Wi-Fi wins when connected; else BLE if fresh; else nothing.
    if (WiFi.status() == WL_CONNECTED)                  g_source = SRC_WIFI;
    else if (now - g_bleLastRx <= BLE_FRESHNESS_MS)     g_source = SRC_BLE;
    else                                                g_source = SRC_NONE;

    handleTouch();
    if (g_view == DETAIL && now - g_lastTouch >= IDLE_RETURN_MS) g_view = RADAR;
```

- [ ] **Step 7: Commit**

```bash
git add src/flight_ticker.ino
git commit -m "feat: variable radar center, non-blocking wifi, source arbitration + indicator"
```

---

## Task 4: Firmware — NimBLE peripheral + packet ingest

**Files:**
- Modify: `src/flight_ticker.ino`

(No host test — verified by compile in Task 6 and on-device in Task 7.)

- [ ] **Step 1: Include the BLE headers**

In `src/flight_ticker.ino`, after `#include "cst816s.h"` add:

```cpp
#include <NimBLEDevice.h>
#include "ble_core.h"
```

- [ ] **Step 2: Add the BLE constants, ingest buffer, and write callback**

After the `onTouchISR` definition (the line `void IRAM_ATTR onTouchISR() { g_touchEvent = true; }`), add:

```cpp
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
```

- [ ] **Step 3: Start the BLE peripheral in `setup()`**

In `setup()`, after the line `attachInterrupt(digitalPinToInterrupt(TOUCH_INT), onTouchISR, FALLING);`, add:

```cpp
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
```

- [ ] **Step 4: Handle received packets in `loop()`**

In `loop()`, immediately after `unsigned long now = millis();`, add:

```cpp
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
```

- [ ] **Step 5: Commit**

```bash
git add src/flight_ticker.ino
git commit -m "feat: NimBLE peripheral + binary packet ingest feeding the radar"
```

---

## Task 5: `scripts/ble_send.py` — test sender

**Files:**
- Create: `scripts/ble_send.py`

(No host test — it's a manual harness, used in Task 7.)

- [ ] **Step 1: Create `scripts/ble_send.py`**

```python
#!/usr/bin/env python3
"""Send one test packet to the Flight Radar device over BLE (sub-project A harness).

Usage: pip install bleak; python3 scripts/ble_send.py
On macOS, the terminal app needs Bluetooth permission (System Settings > Privacy).
Mirror of the wire format in src/ble_core.h.
"""
import asyncio
import struct
from bleak import BleakScanner, BleakClient

DEVICE_NAME = "FlightRadar"
CHAR_UUID   = "f1a90002-7e1d-4c2a-9b3f-1a2b3c4d5e6f"

FLAG_GROUND, FLAG_ALT_VALID, FLAG_GS_VALID = 0x01, 0x02, 0x04


def _field(s: str, n: int) -> bytes:
    b = s.encode("ascii", "ignore")[:n]
    return b + b" " * (n - len(b))


def _record(cs, ty, lat, lon, alt_ft, gs_kt, flags) -> bytes:
    return _field(cs, 8) + _field(ty, 4) + struct.pack("<ffihBB", lat, lon, alt_ft, gs_kt, flags, 0)


def _packet(clat, clon, aircraft) -> bytes:
    pkt = struct.pack("<BBBB", 0x46, 0x52, 1, len(aircraft))  # 'F','R',version,count
    pkt += struct.pack("<ff", clat, clon)
    for a in aircraft:
        pkt += _record(*a)
    return pkt


async def main():
    dev = await BleakScanner.find_device_by_name(DEVICE_NAME, timeout=10)
    if not dev:
        print(f"device '{DEVICE_NAME}' not found"); return
    async with BleakClient(dev) as client:
        clat, clon = 38.7677, -9.3006  # Lisbon-ish center
        aircraft = [
            ("RYR4KP", "B738", 38.80, -9.28, 12000, 420, FLAG_ALT_VALID | FLAG_GS_VALID),
            ("TAP812", "A320", 38.72, -9.40, 35000, 450, FLAG_ALT_VALID | FLAG_GS_VALID),
            ("ABC123", "B772", 38.70, -9.10, 0, 5, FLAG_GROUND),
        ]
        await client.write_gatt_char(CHAR_UUID, _packet(clat, clon, aircraft), response=False)
        print(f"sent {len(aircraft)} aircraft to {DEVICE_NAME}")


if __name__ == "__main__":
    asyncio.run(main())
```

- [ ] **Step 2: Commit**

```bash
git add scripts/ble_send.py
git commit -m "test: add bleak sender stub for on-device BLE verification"
```

---

## Task 6: Compile the firmware

**Files:** none (build verification)

- [ ] **Step 1: Build the esp32-s3 env**

Run: `/opt/homebrew/bin/pio run -e esp32-s3`
Expected: `[SUCCESS]`. NimBLE compiles and links; no undefined references. Note the RAM/Flash percentages printed at the end (NimBLE adds flash and some RAM — confirm Flash stays well under 100% and the build links).

- [ ] **Step 2: If linking fails on the NimBLE callback signature**, the installed NimBLE-Arduino is a 2.x API. Re-pin in `platformio.ini` to `h2zero/NimBLE-Arduino@^1.4.1` exactly (the `onWrite(NimBLECharacteristic*)` single-arg signature in Task 4 is the 1.x API), then `/opt/homebrew/bin/pio run -e esp32-s3` again.

- [ ] **Step 3: Commit** (only if a re-pin was needed)

```bash
git add platformio.ini
git commit -m "build: pin NimBLE-Arduino to 1.x for the onWrite signature"
```

---

## Task 7: On-device verification

**Files:** none (device verification). Board on USB-C at `/dev/cu.usbmodem*`. Requires `pip install bleak` and Bluetooth permission for the terminal.

- [ ] **Step 1: Flash and confirm Wi-Fi source**

Run: `/opt/homebrew/bin/pio run -e esp32-s3 -t upload`
Expected: `[SUCCESS]`. On the radar, the bottom-center indicator shows green **W** (Wi-Fi connected, polling as before). Aircraft still appear.

- [ ] **Step 2: Force Wi-Fi off to exercise the fallback**

Temporarily edit `src/config.h` and set `WIFI_SSID` to a non-existent network (e.g. `"no-such-wifi"`), then re-flash:
`/opt/homebrew/bin/pio run -e esp32-s3 -t upload`
Expected: after the boot Wi-Fi attempt fails, the indicator shows red **NO LINK** and the radar holds its last frame. The sweep keeps animating (the loop no longer blocks on reconnect).

- [ ] **Step 3: Send a BLE packet**

Run: `python3 scripts/ble_send.py`
Expected: prints `sent 3 aircraft to FlightRadar`. On the device the indicator flips to cyan **B**, and the radar re-centers on the Lisbon coordinates showing the injected aircraft (RYR4KP nearest). Tapping opens the detail carousel over the injected flights.

- [ ] **Step 4: Confirm freshness fallback**

Wait ~30 s without sending again.
Expected: once `now - g_bleLastRx` exceeds `BLE_FRESHNESS_MS`, the indicator returns to red **NO LINK** (BLE data went stale).

- [ ] **Step 5: Restore Wi-Fi**

Revert `WIFI_SSID` in `src/config.h` to the real network and re-flash.
Expected: indicator back to green **W**, live aircraft from the API.

- [ ] **Step 6: Note any issues.** If the device crashes/reboots when BLE + Wi-Fi + the sprite are all active (heap exhaustion), drop the framebuffer to 8-bit: change `fb.setColorDepth(16)` to `fb.setColorDepth(8)` in `setup()`, re-flash, and re-test. Commit that change if needed:

```bash
git add src/flight_ticker.ino
git commit -m "fix: 8-bit framebuffer to fit BLE + WiFi heap"
```

---

## Done criteria

- `pio test -e native -f test_core` → 36 tests pass.
- `pio run -e esp32-s3` → builds clean with NimBLE.
- On device: green **W** with Wi-Fi; with Wi-Fi down, `ble_send.py` flips the radar to cyan **B**, re-centers on the packet GPS, and renders the injected aircraft; data goes **NO LINK** after `BLE_FRESHNESS_MS`; restoring Wi-Fi returns to **W**.
