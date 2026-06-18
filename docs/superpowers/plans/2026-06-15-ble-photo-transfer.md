# Aircraft Photos over BLE Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Show aircraft photos in the PHOTO view while the device is on the BLE fallback path, by having the phone fetch the photo and stream it to the device over a new BLE characteristic.

**Architecture:** Pull model. The device (BLE peripheral) NOTIFYs a photo request for the selected aircraft on a new characteristic `f1a90005`; the phone (central) fetches the wsrv.nl-proxied, extra-compressed 240×240 baseline JPEG and WRITEs it back in chunks; the device assembles the bytes in PSRAM and decodes them with the existing JPEGDEC pipeline (extracted into a shared `decodeJpegToCache` helper), publishing the result through the same key-identity + memory-barrier handoff used by the Wi-Fi path.

**Tech Stack:** C++ (Arduino/ESP32, NimBLE-Arduino 1.4.x, JPEGDEC, TFT_eSPI), PlatformIO + Unity host tests; Flutter/Dart companion (flutter_blue_plus, http) + flutter_test.

---

## File Structure

- `src/photo_ble_core.h` — **new**, pure (Arduino-free) BLE photo wire protocol: `buildPhotoReq`, `parsePhotoReq`, `parsePhotoHeader`, `parsePhotoChunk` + constants/structs. Host-tested.
- `test/test_core/test_main.cpp` — **modify**, add host tests + `#include` + `RUN_TEST` lines.
- `src/flight_ticker.ino` — **modify**: extract `decodeJpegToCache`; add the `f1a90005` characteristic, globals, receive callback, the BLE branch in `enterPhotoView`, netTask decode/publish of the received photo, and the loop-side timeout.
- `companion/lib/packet/photo_ble_packet.dart` — **new**, Dart mirror: `parsePhotoReq`, `buildPhotoHeader`, `buildPhotoChunk`, `chunkJpeg`, `buildProxiedPhotoUrl`.
- `companion/test/photo_ble_packet_test.dart` — **new**, Flutter unit tests for the mirror.
- `companion/lib/ble/ble_manager.dart` — **modify**: discover + subscribe `f1a90005`, respond to PR by fetching + streaming the photo.
- `README.md` / `CLAUDE.md` — **modify**: document the BLE photo path.

---

## Task 1: BLE photo wire protocol (pure header + host tests)

**Files:**
- Create: `src/photo_ble_core.h`
- Test: `test/test_core/test_main.cpp`

- [ ] **Step 1: Write `src/photo_ble_core.h`**

```cpp
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>

// Photo-over-BLE wire format (BLE characteristic f1a90005). Pull model:
//   PR (device -> phone, NOTIFY): "request photo for <key>"
//   PH (phone -> device, WRITE):  transfer header (total JPEG length + credit)
//   PD (phone -> device, WRITE):  one JPEG chunk
// Little-endian (both ends are LE). Mirrors the wifi_scan_core.h style.
constexpr uint8_t PHOTOBLE_MAGIC      = 0x50; // 'P'
constexpr uint8_t PHOTOBLE_T_REQ      = 0x52; // 'R'
constexpr uint8_t PHOTOBLE_T_HEADER   = 0x48; // 'H'
constexpr uint8_t PHOTOBLE_T_DATA     = 0x44; // 'D'
constexpr uint8_t PHOTOBLE_VERSION    = 1;
constexpr size_t  PHOTOBLE_MAX_KEY    = 11;     // registration/hex
constexpr size_t  PHOTOBLE_MAX_CRED   = 47;     // photographer string
constexpr size_t  PHOTOBLE_REQ_MAX    = 5 + PHOTOBLE_MAX_KEY;   // hdr(5)+key
constexpr uint32_t PHOTOBLE_MAX_IMG   = 48u * 1024u;            // sanity cap

// PR: 'P','R',ver,reqId,keyLen,key... . Returns bytes written, 0 if invalid.
// buf must hold at least PHOTOBLE_REQ_MAX bytes.
inline size_t buildPhotoReq(uint8_t* buf, uint8_t reqId, const std::string& key) {
    if (!buf || key.empty() || key.size() > PHOTOBLE_MAX_KEY) return 0;
    buf[0] = PHOTOBLE_MAGIC;
    buf[1] = PHOTOBLE_T_REQ;
    buf[2] = PHOTOBLE_VERSION;
    buf[3] = reqId;
    buf[4] = static_cast<uint8_t>(key.size());
    std::memcpy(buf + 5, key.data(), key.size());
    return 5 + key.size();
}

struct PhotoReq { bool ok = false; uint8_t reqId = 0; std::string key; };

inline PhotoReq parsePhotoReq(const uint8_t* buf, size_t len) {
    PhotoReq r;
    if (!buf || len < 5) return r;
    if (buf[0] != PHOTOBLE_MAGIC || buf[1] != PHOTOBLE_T_REQ) return r;
    if (buf[2] != PHOTOBLE_VERSION) return r;
    uint8_t keyLen = buf[4];
    if (keyLen == 0 || keyLen > PHOTOBLE_MAX_KEY || len < 5u + keyLen) return r;
    r.reqId = buf[3];
    r.key.assign(reinterpret_cast<const char*>(buf + 5), keyLen);
    r.ok = true;
    return r;
}

struct PhotoHeader {
    bool ok = false; uint8_t reqId = 0; uint32_t totalLen = 0; std::string credit;
};

// PH: 'P','H',ver,reqId,totalLen(u32 LE),credLen,cred...
inline PhotoHeader parsePhotoHeader(const uint8_t* buf, size_t len) {
    PhotoHeader h;
    if (!buf || len < 9) return h;
    if (buf[0] != PHOTOBLE_MAGIC || buf[1] != PHOTOBLE_T_HEADER) return h;
    if (buf[2] != PHOTOBLE_VERSION) return h;
    uint32_t total;
    std::memcpy(&total, buf + 4, 4);
    uint8_t credLen = buf[8];
    if (credLen > PHOTOBLE_MAX_CRED || len < 9u + credLen) return h;
    h.reqId = buf[3];
    h.totalLen = total;
    if (credLen) h.credit.assign(reinterpret_cast<const char*>(buf + 9), credLen);
    h.ok = true;
    return h;
}

struct PhotoChunk {
    bool ok = false; uint8_t reqId = 0; uint16_t seq = 0;
    const uint8_t* data = nullptr; size_t dataLen = 0;
};

// PD: 'P','D',ver,reqId,seq(u16 LE),bytes... . `data` points into `buf`.
inline PhotoChunk parsePhotoChunk(const uint8_t* buf, size_t len) {
    PhotoChunk c;
    if (!buf || len < 6) return c;
    if (buf[0] != PHOTOBLE_MAGIC || buf[1] != PHOTOBLE_T_DATA) return c;
    if (buf[2] != PHOTOBLE_VERSION) return c;
    uint16_t seq;
    std::memcpy(&seq, buf + 4, 2);
    c.reqId = buf[3];
    c.seq = seq;
    c.data = buf + 6;
    c.dataLen = len - 6;
    c.ok = true;
    return c;
}
```

- [ ] **Step 2: Add tests to `test/test_core/test_main.cpp`**

Add the include near the other core includes (after `#include "../../src/photo_core.h"`):

```cpp
#include "../../src/photo_ble_core.h"
```

Add these test functions (place them next to the other tests, e.g. after the scan tests):

```cpp
void test_photoble_req_roundtrip() {
    uint8_t buf[PHOTOBLE_REQ_MAX];
    size_t n = buildPhotoReq(buf, 7, "ABC-123");
    TEST_ASSERT_EQUAL_UINT32(5 + 7, n);
    PhotoReq r = parsePhotoReq(buf, n);
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_EQUAL_UINT8(7, r.reqId);
    TEST_ASSERT_EQUAL_STRING("ABC-123", r.key.c_str());
}

void test_photoble_req_rejects() {
    uint8_t buf[PHOTOBLE_REQ_MAX];
    TEST_ASSERT_EQUAL_UINT32(0, buildPhotoReq(buf, 1, ""));                  // empty key
    TEST_ASSERT_EQUAL_UINT32(0, buildPhotoReq(buf, 1, "TOOLONGKEY12"));      // 12 > max 11
    uint8_t bad[5] = {0x50, 0x52, 1, 1, 0};                                  // keyLen 0
    TEST_ASSERT_FALSE(parsePhotoReq(bad, 5).ok);
    uint8_t wrongmagic[6] = {0x50, 0x48, 1, 1, 1, 'A'};
    TEST_ASSERT_FALSE(parsePhotoReq(wrongmagic, 6).ok);
}

void test_photoble_header_parse() {
    uint8_t buf[64];
    buf[0] = 0x50; buf[1] = 0x48; buf[2] = 1; buf[3] = 9;
    uint32_t total = 5000; std::memcpy(buf + 4, &total, 4);
    const char* cred = "Jane Doe"; buf[8] = (uint8_t)strlen(cred);
    std::memcpy(buf + 9, cred, strlen(cred));
    PhotoHeader h = parsePhotoHeader(buf, 9 + strlen(cred));
    TEST_ASSERT_TRUE(h.ok);
    TEST_ASSERT_EQUAL_UINT8(9, h.reqId);
    TEST_ASSERT_EQUAL_UINT32(5000, h.totalLen);
    TEST_ASSERT_EQUAL_STRING("Jane Doe", h.credit.c_str());
}

void test_photoble_header_zero_len_and_rejects() {
    uint8_t z[9] = {0x50, 0x48, 1, 3, 0, 0, 0, 0, 0};   // totalLen 0, credLen 0
    PhotoHeader h = parsePhotoHeader(z, 9);
    TEST_ASSERT_TRUE(h.ok);
    TEST_ASSERT_EQUAL_UINT32(0, h.totalLen);
    TEST_ASSERT_FALSE(parsePhotoHeader(z, 8).ok);                 // too short
    uint8_t badcred[9] = {0x50, 0x48, 1, 1, 0x10, 0, 0, 0, 60};   // credLen 60 > max
    TEST_ASSERT_FALSE(parsePhotoHeader(badcred, 9).ok);
}

void test_photoble_chunk_parse() {
    uint8_t buf[10] = {0x50, 0x44, 1, 4, 0x02, 0x00, 0xDE, 0xAD, 0xBE, 0xEF};
    PhotoChunk c = parsePhotoChunk(buf, 10);
    TEST_ASSERT_TRUE(c.ok);
    TEST_ASSERT_EQUAL_UINT8(4, c.reqId);
    TEST_ASSERT_EQUAL_UINT16(2, c.seq);
    TEST_ASSERT_EQUAL_UINT32(4, c.dataLen);
    TEST_ASSERT_EQUAL_UINT8(0xDE, c.data[0]);
    TEST_ASSERT_EQUAL_UINT8(0xEF, c.data[3]);
    TEST_ASSERT_FALSE(parsePhotoChunk(buf, 5).ok);               // too short for header
}
```

Register them in the Unity runner (the `main()` that lists `RUN_TEST(...)`):

```cpp
    RUN_TEST(test_photoble_req_roundtrip);
    RUN_TEST(test_photoble_req_rejects);
    RUN_TEST(test_photoble_header_parse);
    RUN_TEST(test_photoble_header_zero_len_and_rejects);
    RUN_TEST(test_photoble_chunk_parse);
```

- [ ] **Step 3: Run host tests — expect FAIL first, then PASS**

Run: `pio test -e native -f test_core`
Expected: PASS, total count rises by 5 (was 63 → 68). If it fails to compile, fix `photo_ble_core.h`.

- [ ] **Step 4: Commit**

```bash
git add src/photo_ble_core.h test/test_core/test_main.cpp
git commit -m "feat(firmware): photo_ble_core.h — BLE photo wire protocol (PR/PH/PD) + host tests"
```

---

## Task 2: Extract `decodeJpegToCache` helper (firmware refactor, no behavior change)

**Files:**
- Modify: `src/flight_ticker.ino` (inside `fetchPhoto`, and add the new function just above it)

- [ ] **Step 1: Add the helper above `fetchPhoto`**

Find `PhotoResult fetchPhoto(const Aircraft& ac) {` and insert this function immediately BEFORE it:

```cpp
// Decode a baseline JPEG in `buf` (len bytes) into a fresh PSRAM 240x240 RGB565
// frame, insert it into the LRU cache under `key`, and return it. Undecodable
// images are negative-cached. netTask-only (owns g_jpeg + the cache). Shared by
// the Wi-Fi fetch and the BLE photo-receive path.
PhotoResult decodeJpegToCache(const uint8_t* buf, size_t len,
                              const std::string& key, const std::string& credit) {
    PhotoResult res;
    uint16_t* px = (uint16_t*)heap_caps_malloc(240 * 240 * 2, MALLOC_CAP_SPIRAM);
    if (!px) return res;
    std::memset(px, 0, 240 * 240 * 2);   // black letterbox margins
    if (g_jpeg.openRAM((uint8_t*)buf, (int)len, photoDrawCb)) {
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
            photoCacheInsert(key, px, credit);
            res.ok = true; res.px = px; res.photographer = credit;
        } else {
            heap_caps_free(px);
            g_photoMiss[key] = true;
        }
    } else {
        heap_caps_free(px);
        g_photoMiss[key] = true;
    }
    return res;
}
```

- [ ] **Step 2: Replace the decode tail inside `fetchPhoto`**

In `fetchPhoto`, replace everything from `uint16_t* px = (uint16_t*)heap_caps_malloc(240 * 240 * 2, MALLOC_CAP_SPIRAM);` through the final `return res;` (the `if (g_jpeg.openRAM(...)) { ... } else { ... }` block) with:

```cpp
    return decodeJpegToCache(g_photoDlBuf, ilen, key, meta.photographer);
```

So the end of `fetchPhoto` reads:

```cpp
    std::string imgUrl = buildProxiedPhotoUrl(meta.url);
    if (!httpsGetToBuf(imgUrl.c_str(), g_photoDlBuf, PHOTO_DL_MAX, &ilen)) return res;  // transient: don't negative-cache
    return decodeJpegToCache(g_photoDlBuf, ilen, key, meta.photographer);
}
```

- [ ] **Step 3: Build firmware (decode path isn't host-tested) + run host tests**

Run: `pio run -e esp32-s3`
Expected: SUCCESS.
Run: `pio test -e native -f test_core`
Expected: PASS (68, unchanged).

- [ ] **Step 4: Commit**

```bash
git add src/flight_ticker.ino
git commit -m "refactor(firmware): extract decodeJpegToCache from fetchPhoto (shared by Wi-Fi + BLE)"
```

---

## Task 3: GATT characteristic, globals, and receive callback (firmware)

**Files:**
- Modify: `src/flight_ticker.ino`

- [ ] **Step 1: Add the include + UUID + constants**

Near the top includes, after `#include "ble_core.h"` (or alongside the other core includes), add:

```cpp
#include "photo_ble_core.h"
```

Next to the other UUID constants (`BLE_WIFISCAN_UUID = "f1a90004..."`), add:

```cpp
static const char* BLE_PHOTO_UUID = "f1a90005-7e1d-4c2a-9b3f-1a2b3c4d5e6f";
```

- [ ] **Step 2: Add the BLE-photo globals**

Near the other photo globals (after `unsigned long g_photoMsgUntil = ...;`), add:

```cpp
// --- Photo over BLE (phone-streamed; used only when Wi-Fi is down) ---
static const size_t   BLE_PHOTO_MAX = 48 * 1024;          // == PHOTOBLE_MAX_IMG
static const unsigned long BLE_PHOTO_TIMEOUT_MS = 8000;
NimBLEServer*         g_bleServer = nullptr;              // to count connected centrals
NimBLECharacteristic* g_photoBleChar = nullptr;
uint8_t*              g_blePhotoBuf = nullptr;            // PSRAM, allocated once in setup()
volatile uint8_t      g_blePhotoReqId = 0;               // current request id (loop-set)
char                  g_blePhotoKey[12] = "";            // aircraft of the current request (loop-set)
volatile uint32_t     g_blePhotoExpected = 0;            // totalLen from PH (callback-set)
volatile uint32_t     g_blePhotoGot = 0;                 // bytes received (callback-set)
char                  g_blePhotoCredit[48] = "";         // photographer from PH (callback-set)
volatile bool         g_blePhotoReady = false;           // full image received → netTask decodes
volatile bool         g_blePhotoNone = false;            // PH totalLen==0 → no photo
unsigned long         g_blePhotoDeadline = 0;            // loop-owned timeout
```

- [ ] **Step 3: Add the receive callback class**

Next to the other NimBLE callback classes (e.g. after `WifiScanCallbacks`), add:

```cpp
// Receives PH/PD frames (phone -> device) on f1a90005. Only buffers + flags;
// netTask decodes. Frames whose reqId != the in-flight g_blePhotoReqId are
// dropped (a stale stream from a previous swipe).
class PhotoBleCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c) override {
        std::string v = c->getValue();
        const uint8_t* p = (const uint8_t*)v.data();
        size_t n = v.size();

        PhotoHeader h = parsePhotoHeader(p, n);
        if (h.ok) {
            if (h.reqId != g_blePhotoReqId) return;            // stale request
            if (h.totalLen == 0 || h.totalLen > BLE_PHOTO_MAX) {
                g_blePhotoNone = true;                          // no photo / oversize
                return;
            }
            g_blePhotoExpected = h.totalLen;
            g_blePhotoGot = 0;
            strlcpy(g_blePhotoCredit, h.credit.c_str(), sizeof(g_blePhotoCredit));
            return;
        }

        PhotoChunk ch = parsePhotoChunk(p, n);
        if (ch.ok) {
            if (ch.reqId != g_blePhotoReqId) return;           // stale
            if (!g_blePhotoBuf || g_blePhotoExpected == 0) return;
            if (g_blePhotoGot + ch.dataLen > g_blePhotoExpected) return;  // overflow guard
            std::memcpy(g_blePhotoBuf + g_blePhotoGot, ch.data, ch.dataLen);
            g_blePhotoGot += ch.dataLen;
            if (g_blePhotoGot >= g_blePhotoExpected) {
                __sync_synchronize();      // publish the buffer before the ready flag
                g_blePhotoReady = true;
            }
        }
    }
};
```

- [ ] **Step 4: Create the characteristic + buffer in `setup()`**

In `setup()`, find where the server/service is created. Change the local server to the global and add the characteristic. Replace:

```cpp
    NimBLEServer* bleServer = NimBLEDevice::createServer();
```

with:

```cpp
    g_bleServer = NimBLEDevice::createServer();
    NimBLEServer* bleServer = g_bleServer;
```

After the `g_wifiScanChar` creation block (before `bleSvc->start();`), add:

```cpp
    g_photoBleChar = bleSvc->createCharacteristic(
        BLE_PHOTO_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::NOTIFY);
    g_photoBleChar->setCallbacks(new PhotoBleCallbacks());
```

After `if (!fb.createSprite(240, 240)) ...` (anywhere in setup after PSRAM is up), add the one-time buffer alloc:

```cpp
    g_blePhotoBuf = (uint8_t*)heap_caps_malloc(BLE_PHOTO_MAX, MALLOC_CAP_SPIRAM);
    if (!g_blePhotoBuf) Serial.println("ble photo buf alloc failed");
```

- [ ] **Step 5: Build**

Run: `pio run -e esp32-s3`
Expected: SUCCESS.

- [ ] **Step 6: Commit**

```bash
git add src/flight_ticker.ino
git commit -m "feat(firmware): f1a90005 photo characteristic + PH/PD receive into PSRAM buffer"
```

---

## Task 4: Request on swipe, decode on netTask, timeout in loop (firmware)

**Files:**
- Modify: `src/flight_ticker.ino` (`enterPhotoView`, `netTask`, `loop`)

- [ ] **Step 1: Add the BLE branch to `enterPhotoView`**

In `enterPhotoView`, replace the existing no-Wi-Fi block:

```cpp
    if (WiFi.status() != WL_CONNECTED) {
        flashPhotoMsg("No Wi-Fi");
        drainTouch();
        return;
    }
```

with:

```cpp
    if (WiFi.status() != WL_CONNECTED) {
        // BLE photo path: the phone (which has internet) fetches + streams it.
        // Requires a connected central and the photo characteristic.
        if (g_source == SRC_BLE && g_bleServer && g_bleServer->getConnectedCount() > 0
            && g_photoBleChar && g_blePhotoBuf) {
            std::string key = ac.registration.empty() ? ac.hex : ac.registration;
            if (!key.empty()) {
                g_blePhotoReqId++;                 // new request; invalidates stale streams
                g_blePhotoExpected = 0;
                g_blePhotoGot = 0;
                g_blePhotoReady = false;
                g_blePhotoNone = false;
                g_blePhotoCredit[0] = '\0';
                strlcpy(g_blePhotoKey, key.c_str(), sizeof(g_blePhotoKey));
                uint8_t reqBuf[PHOTOBLE_REQ_MAX];
                size_t rn = buildPhotoReq(reqBuf, g_blePhotoReqId, key);
                if (rn) { g_photoBleChar->setValue(reqBuf, rn); g_photoBleChar->notify(); }
                // Same loading + identity state the Wi-Fi path uses.
                g_photoReady = false;
                strlcpy(g_photoReqKey, key.c_str(), sizeof(g_photoReqKey));
                g_photoLoading = true;
                g_photoPx = nullptr;
                g_photoMsgUntil = 0;
                g_blePhotoDeadline = millis() + BLE_PHOTO_TIMEOUT_MS;
                g_view = PHOTO;
                return;
            }
        }
        flashPhotoMsg("No Wi-Fi");
        drainTouch();
        return;
    }
```

- [ ] **Step 2: Decode + publish in `netTask`**

In `netTask`, after the existing `if (g_photoReq) { ... }` block (the Wi-Fi fetch) and before `vTaskDelay(...)`, add:

```cpp
        if (g_blePhotoReady) {
            g_blePhotoReady = false;
            __sync_synchronize();   // acquire: see the bytes the BLE callback wrote
            PhotoResult r = decodeJpegToCache(g_blePhotoBuf, g_blePhotoExpected,
                                              g_blePhotoKey, g_blePhotoCredit);
            g_photoResPx = r.px;
            strlcpy(g_photoResCredit, r.photographer.c_str(), sizeof(g_photoResCredit));
            strlcpy(g_photoResKey, g_blePhotoKey, sizeof(g_photoResKey));
            g_photoResOk = r.ok;
            __sync_synchronize();   // publish results before the ready flag
            g_photoReady = true;
        }
        if (g_blePhotoNone) {
            g_blePhotoNone = false;
            g_photoResPx = nullptr;
            g_photoResOk = false;
            strlcpy(g_photoResKey, g_blePhotoKey, sizeof(g_photoResKey));
            __sync_synchronize();
            g_photoReady = true;
        }
```

- [ ] **Step 3: Add the BLE-photo timeout + clear deadline on consume, in `loop`**

In `loop`, in the `if (g_photoReady) { ... }` consume block, after `g_photoLoading = false;` add `g_blePhotoDeadline = 0;`:

```cpp
        if (g_view == PHOTO && g_photoLoading && matches) {
            g_photoLoading = false;
            g_blePhotoDeadline = 0;
            if (g_photoResOk) {
```

Then, immediately AFTER that whole `if (g_photoReady) { ... }` block, add the timeout:

```cpp
    // BLE photo never arrived (phone offline / no photo / lost) → stop "Loading".
    if (g_blePhotoDeadline && millis() > g_blePhotoDeadline) {
        g_blePhotoDeadline = 0;
        if (g_view == PHOTO && g_photoLoading) {
            g_photoLoading = false;
            g_photoMsgUntil = millis() + 1500;   // "No photo"
        }
    }
```

- [ ] **Step 4: Build + host tests**

Run: `pio run -e esp32-s3`
Expected: SUCCESS.
Run: `pio test -e native -f test_core`
Expected: PASS (68).

- [ ] **Step 5: Commit**

```bash
git add src/flight_ticker.ino
git commit -m "feat(firmware): BLE photo request on swipe, netTask decode, loop timeout"
```

---

## Task 5: Companion wire mirror + proxied URL (Dart + tests)

**Files:**
- Create: `companion/lib/packet/photo_ble_packet.dart`
- Create: `companion/test/photo_ble_packet_test.dart`

- [ ] **Step 1: Write `companion/lib/packet/photo_ble_packet.dart`**

```dart
import 'dart:convert';
import 'dart:typed_data';

// Mirror of src/photo_ble_core.h (byte-exact). Characteristic f1a90005.
const int photoBleMagic = 0x50; // 'P'
const int photoBleTReq = 0x52; // 'R'
const int photoBleTHeader = 0x48; // 'H'
const int photoBleTData = 0x44; // 'D'
const int photoBleVersion = 1;
const int photoBleMaxKey = 11;
const int photoBleMaxCred = 47;

/// A parsed photo request (device -> phone).
class PhotoReq {
  final int reqId;
  final String key;
  const PhotoReq(this.reqId, this.key);
}

/// Parse a PR notify. Returns null on malformed bytes.
PhotoReq? parsePhotoReq(List<int> bytes) {
  if (bytes.length < 5) return null;
  if (bytes[0] != photoBleMagic || bytes[1] != photoBleTReq) return null;
  if (bytes[2] != photoBleVersion) return null;
  final reqId = bytes[3];
  final keyLen = bytes[4];
  if (keyLen == 0 || keyLen > photoBleMaxKey) return null;
  if (bytes.length < 5 + keyLen) return null;
  final key = utf8.decode(bytes.sublist(5, 5 + keyLen), allowMalformed: true);
  return PhotoReq(reqId, key);
}

/// Build a PH header frame. `credit` is truncated to photoBleMaxCred bytes.
Uint8List buildPhotoHeader(int reqId, int totalLen, String credit) {
  final cred = utf8.encode(credit);
  final c = cred.length > photoBleMaxCred ? cred.sublist(0, photoBleMaxCred) : cred;
  final b = BytesBuilder();
  b.add([photoBleMagic, photoBleTHeader, photoBleVersion, reqId & 0xFF]);
  final lenBytes = Uint8List(4)
    ..buffer.asByteData().setUint32(0, totalLen, Endian.little);
  b.add(lenBytes);
  b.add([c.length]);
  b.add(c);
  return b.toBytes();
}

/// Build a PD data frame carrying [chunk] with sequence [seq].
Uint8List buildPhotoChunk(int reqId, int seq, List<int> chunk) {
  final b = BytesBuilder();
  b.add([photoBleMagic, photoBleTData, photoBleVersion, reqId & 0xFF]);
  final seqBytes = Uint8List(2)
    ..buffer.asByteData().setUint16(0, seq, Endian.little);
  b.add(seqBytes);
  b.add(chunk);
  return b.toBytes();
}

/// Split [jpeg] into PD frames whose payload is at most [maxPayload] bytes.
List<Uint8List> chunkJpeg(int reqId, List<int> jpeg, int maxPayload) {
  final out = <Uint8List>[];
  var seq = 0;
  for (var off = 0; off < jpeg.length; off += maxPayload) {
    final end = (off + maxPayload < jpeg.length) ? off + maxPayload : jpeg.length;
    out.add(buildPhotoChunk(reqId, seq, jpeg.sublist(off, end)));
    seq++;
  }
  return out;
}

/// Mirror of photo_core.h buildProxiedPhotoUrl, plus a quality knob for BLE.
/// wsrv.nl re-encodes the (progressive) planespotters thumb into a baseline,
/// cover-cropped 240x240 JPEG; lower [quality] shrinks the BLE transfer.
String buildProxiedPhotoUrl(String src, {int quality = 55}) {
  var bare = src;
  if (bare.startsWith('https://')) {
    bare = bare.substring(8);
  } else if (bare.startsWith('http://')) {
    bare = bare.substring(7);
  }
  return 'https://wsrv.nl/?url=$bare&w=240&h=240&fit=cover&output=jpg&q=$quality';
}
```

- [ ] **Step 2: Write `companion/test/photo_ble_packet_test.dart`**

```dart
import 'package:flutter_test/flutter_test.dart';
import 'package:flight_radar_companion/packet/photo_ble_packet.dart';

void main() {
  test('parses a PR request', () {
    final bytes = [0x50, 0x52, 1, 9, 7, ...'ABC-123'.codeUnits];
    final r = parsePhotoReq(bytes)!;
    expect(r.reqId, 9);
    expect(r.key, 'ABC-123');
  });

  test('rejects malformed PR', () {
    expect(parsePhotoReq([]), isNull);
    expect(parsePhotoReq([0x50, 0x52, 1, 1]), isNull);              // no keyLen byte/key
    expect(parsePhotoReq([0x50, 0x48, 1, 1, 1, 65]), isNull);       // wrong type
    expect(parsePhotoReq([0x50, 0x52, 2, 1, 1, 65]), isNull);       // wrong version
    expect(parsePhotoReq([0x50, 0x52, 1, 1, 0]), isNull);           // zero keyLen
    expect(parsePhotoReq([0x50, 0x52, 1, 1, 5, 65]), isNull);       // truncated key
  });

  test('buildPhotoHeader encodes len LE + credit', () {
    final h = buildPhotoHeader(3, 5000, 'Jane');
    expect(h.sublist(0, 4), [0x50, 0x48, 1, 3]);
    expect(h[4], 5000 & 0xFF);          // 0x88
    expect(h[5], (5000 >> 8) & 0xFF);   // 0x13
    expect(h[6], 0); expect(h[7], 0);
    expect(h[8], 'Jane'.length);
    expect(String.fromCharCodes(h.sublist(9)), 'Jane');
  });

  test('buildPhotoChunk encodes seq LE + payload', () {
    final c = buildPhotoChunk(4, 2, [0xDE, 0xAD]);
    expect(c.sublist(0, 4), [0x50, 0x44, 1, 4]);
    expect(c[4], 2); expect(c[5], 0);
    expect(c.sublist(6), [0xDE, 0xAD]);
  });

  test('chunkJpeg splits by maxPayload with rising seq', () {
    final jpeg = List<int>.generate(250, (i) => i & 0xFF);
    final frames = chunkJpeg(7, jpeg, 100);
    expect(frames.length, 3);                 // 100 + 100 + 50
    expect(frames[0][4], 0);                  // seq 0
    expect(frames[2][4], 2);                  // seq 2
    expect(frames[2].length, 6 + 50);
  });

  test('buildProxiedPhotoUrl strips scheme + adds quality', () {
    final u = buildProxiedPhotoUrl('https://t.plnspttrs.net/x/y.jpg', quality: 55);
    expect(u, 'https://wsrv.nl/?url=t.plnspttrs.net/x/y.jpg&w=240&h=240&fit=cover&output=jpg&q=55');
  });
}
```

- [ ] **Step 3: Run Flutter tests**

Run: `cd companion && flutter test test/photo_ble_packet_test.dart`
Expected: All tests pass.

- [ ] **Step 4: Commit**

```bash
git add companion/lib/packet/photo_ble_packet.dart companion/test/photo_ble_packet_test.dart
git commit -m "feat(companion): photo_ble_packet.dart — PR/PH/PD mirror + proxied URL + tests"
```

---

## Task 6: Companion responds to photo requests (BleManager)

**Files:**
- Modify: `companion/lib/ble/ble_manager.dart`

- [ ] **Step 1: Add imports + photo deps to `BleManager`**

At the top of `ble_manager.dart` add:

```dart
import 'package:http/http.dart' as http;
import '../packet/photo_ble_packet.dart';
import '../data/photo_client.dart';
```

Add the photo UUID constant next to the existing UUIDs in the class:

```dart
  static final Guid photoUuid = Guid('f1a90005-7e1d-4c2a-9b3f-1a2b3c4d5e6f');
```

Add fields to the class (next to `_char`):

```dart
  BluetoothCharacteristic? _photoChar;
  StreamSubscription<List<int>>? _photoSub;
  final PhotoClient _photoClient = PhotoClient();
  final http.Client _http = http.Client();
```

- [ ] **Step 2: Discover + subscribe the photo characteristic in `_connect`**

In `_connect`, inside the service-discovery loop, also capture the photo characteristic. Replace:

```dart
      for (final s in services) {
        if (s.uuid == serviceUuid) {
          for (final c in s.characteristics) {
            if (c.uuid == charUuid) _char = c;
          }
        }
      }
```

with:

```dart
      _photoChar = null;
      for (final s in services) {
        if (s.uuid == serviceUuid) {
          for (final c in s.characteristics) {
            if (c.uuid == charUuid) _char = c;
            if (c.uuid == photoUuid) _photoChar = c;
          }
        }
      }
```

After the `_set(BleStatus.connected);` line (only reached on a fully successful connect), subscribe to photo requests:

```dart
      await _subscribePhoto();
```

- [ ] **Step 3: Add the photo responder methods**

Add these methods to `BleManager`:

```dart
  Future<void> _subscribePhoto() async {
    final pc = _photoChar;
    if (pc == null) return; // older firmware without the photo characteristic
    try {
      await _photoSub?.cancel();
      await pc.setNotifyValue(true);
      _photoSub = pc.onValueReceived.listen((bytes) {
        final req = parsePhotoReq(bytes);
        if (req != null) _servePhoto(req); // fire-and-forget
      });
    } catch (_) {/* photo feature stays off this session */}
  }

  // Fetch the requested aircraft's photo and stream it back. The device key is a
  // registration or a hex; try it as both (reg first, then hex).
  Future<void> _servePhoto(PhotoReq req) async {
    final pc = _photoChar;
    if (pc == null) return;
    try {
      final ref = await _photoClient.lookup(reg: req.key, hex: req.key);
      if (ref == null) {
        await pc.write(buildPhotoHeader(req.reqId, 0, ''), withoutResponse: false);
        return;
      }
      final url = buildProxiedPhotoUrl(ref.thumbUrl, quality: 55);
      final resp =
          await _http.get(Uri.parse(url)).timeout(const Duration(seconds: 10));
      if (resp.statusCode != 200 || resp.bodyBytes.isEmpty) {
        await pc.write(buildPhotoHeader(req.reqId, 0, ''), withoutResponse: false);
        return;
      }
      final jpeg = resp.bodyBytes;
      await pc.write(
          buildPhotoHeader(req.reqId, jpeg.length, ref.photographer),
          withoutResponse: false);
      // Conservative payload from the negotiated MTU (ATT 3 + our 6-byte header).
      final device = _device;
      final mtu = device?.mtuNow ?? 23;
      final payload = (mtu - 9).clamp(20, 500);
      for (final frame in chunkJpeg(req.reqId, jpeg, payload)) {
        await pc.write(frame, withoutResponse: false); // ordered + flow-controlled
      }
    } catch (_) {
      try {
        await pc.write(buildPhotoHeader(req.reqId, 0, ''), withoutResponse: false);
      } catch (_) {}
    }
  }
```

- [ ] **Step 4: Clean up in `stop`**

In `stop()`, after `await _connSub?.cancel();` add:

```dart
    await _photoSub?.cancel();
    _photoChar = null;
```

- [ ] **Step 5: Analyze + run companion tests**

Run: `cd companion && flutter analyze`
Expected: No new errors.
Run: `cd companion && flutter test`
Expected: All existing tests + the new packet tests pass.

- [ ] **Step 6: Commit**

```bash
git add companion/lib/ble/ble_manager.dart
git commit -m "feat(companion): respond to BLE photo requests — fetch + stream JPEG to device"
```

---

## Task 7: End-to-end verification + docs

**Files:**
- Modify: `README.md`, `CLAUDE.md`

- [ ] **Step 1: Flash firmware + build/run the companion**

```bash
pio run -e esp32-s3 -t upload
cd companion && flutter run   # on a phone with the app's BLE feed active
```

- [ ] **Step 2: Manual on-device verification**

- Put the device in BLE mode (turn Wi-Fi off / out of range) until the source indicator shows **B** (cyan).
- With the companion app connected and feeding, tap an aircraft → swipe up.
- Expected: "Loading photo..." then the photo appears within a few seconds, with the `(c) <photographer> / planespotters.net` credit.
- Swipe through several aircraft: each shows ITS photo (never the previous one).
- An aircraft with no planespotters photo shows "No photo" and recovers (next swipe works).
- With the app NOT connected, swipe up in BLE mode shows "No Wi-Fi" (no hang).

- [ ] **Step 3: Update docs**

In `README.md` (BLE fallback section) and `CLAUDE.md`, add a sentence:

> Photos also work over BLE: in BLE mode a swipe-up sends a photo request
> (`f1a90005`); the companion app fetches the wsrv.nl-proxied 240×240 JPEG and
> streams it back, decoded on-device by the shared `decodeJpegToCache` path.

- [ ] **Step 4: Commit**

```bash
git add README.md CLAUDE.md
git commit -m "docs: photos over BLE (f1a90005 request/stream path)"
```

---

## Self-Review Notes

- **Spec coverage:** wire protocol (Task 1), new characteristic (Task 3), enterPhotoView BLE branch (Task 4), receive into PSRAM (Task 3), shared decode helper (Task 2), netTask decode + existing key/barrier handoff (Task 4), timeout (Task 4), companion subscribe + fetch-compressed + stream (Tasks 5-6), credit/attribution (PH carries it, rendered by existing drawPhoto), host + Flutter tests, manual verify (Task 7). All spec sections map to a task.
- **Type consistency:** `reqId/totalLen/seq/credit/key` names and frame layouts are identical across `photo_ble_core.h`, `photo_ble_packet.dart`, and the firmware callback. `decodeJpegToCache(buf,len,key,credit)` signature is used identically in Task 2 (definition + fetchPhoto call) and Task 4 (netTask call). `g_photoReqKey`/`g_photoResKey`/`g_photoReady` reuse the names added in the photo-race fix.
- **Verify before coding:** confirm the installed NimBLE-Arduino 1.4.x `NimBLECharacteristicCallbacks::onWrite(NimBLECharacteristic*)` single-arg signature matches `PhotoBleCallbacks` (it already matches the existing `WifiScanCallbacks`), and that `flutter_blue_plus` exposes `device.mtuNow` (else use `device.mtu`).
