# Device Aircraft Photos Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Swipe up on the device's aircraft detail page to see its planespotters photo on the round 240×240 display, with a PSRAM LRU cache.

**Architecture:** New `PHOTO` view in the firmware's RADAR⇄DETAIL state machine. On entry: planespotters API lookup (reg-then-hex, descriptive UA), HTTPS JPEG download into PSRAM, JPEGDEC streaming decode (scaled + center-cropped via host-tested math in `src/photo_core.h`) into a 240×240 RGB565 PSRAM framebuffer, cached LRU×8 by reg/hex. Fetch+decode block `loop()` ~1–3 s — accepted house pattern (provisioning blocks 12 s). Companion app untouched.

**Tech Stack:** ESP32-S3 Arduino (PlatformIO), JPEGDEC (bitbank2), ArduinoJson (already a dep, host-testable), TFT_eSPI sprite, Unity host tests.

**Spec:** `docs/superpowers/specs/2026-06-04-device-aircraft-photos-design.md`

**Conventions that bind every task:**
- Host tests: `pio test -e native -f test_core` from the repo root (56 cases currently green). Test includes use `#include "../../src/<header>.h"`.
- Firmware build: `pio run -e esp32-s3`.
- Commit after every green task, from the repo root.
- Line numbers in `src/flight_ticker.ino` are approximate — anchor on symbols.

---

### Task 1: `hex` field on the firmware `Aircraft` (host-tested)

**Files:**
- Modify: `src/flight_core.h` (struct ~line 28, parseNearest body)
- Modify: `test/test_core/test_main.cpp` (1 test + RUN_TEST)

- [ ] **Step 1: Write the failing test**

In `test/test_core/test_main.cpp` (the file's `SAMPLE_JSON` already carries hex values `3c6abc`/`abc123`/`def456`), add:

```cpp
void test_parse_nearest_hex(void) {
    auto list = parseNearest(SAMPLE_JSON, 48.0, 11.0, 5);
    TEST_ASSERT_EQUAL_UINT32(3, list.size());
    TEST_ASSERT_EQUAL_STRING("3c6abc", list[0].hex.c_str());  // nearest = DLH4AB
}
```

and register it: `RUN_TEST(test_parse_nearest_hex);`

- [ ] **Step 2: Run to verify it fails**

Run: `pio test -e native -f test_core`
Expected: compile error — `'struct Aircraft' has no member named 'hex'`

- [ ] **Step 3: Implement**

In `src/flight_core.h`:
- Add to the struct, after `registration`:

```cpp
    std::string hex;          // ICAO24 lowercase ("" if absent; JSON path only)
```

- In `parseNearest`, where the other string fields are read from each JSON
  object (find the lines assigning `a.callsign = trimStr(o["flight"])` /
  `a.type` / `a.registration` — match that local style), add:

```cpp
        a.hex = trimStr(o["hex"]);
```

(`o` = the per-aircraft JsonObject variable used there; keep the file's
actual variable name.)

- [ ] **Step 4: Run to verify it passes**

Run: `pio test -e native -f test_core`
Expected: 57/57 PASS

- [ ] **Step 5: Commit**

```bash
git add src/flight_core.h test/test_core/test_main.cpp
git commit -m "feat(firmware): carry ICAO24 hex through parseNearest (photo lookup fallback)"
```

---

### Task 2: `src/photo_core.h` — planespotters parse + scale/crop math (host-tested)

**Files:**
- Create: `src/photo_core.h`
- Modify: `test/test_core/test_main.cpp` (include + 3 tests + RUN_TESTs)

- [ ] **Step 1: Write the failing tests**

Add `#include "../../src/photo_core.h"` next to the other core includes. Add:

```cpp
void test_parse_planespotters_photo(void) {
    // happy path: thumbnail_large preferred
    const char* ok =
      "{\"photos\":[{\"id\":\"1\",\"thumbnail\":{\"src\":\"https://t/small.jpg\"},"
      "\"thumbnail_large\":{\"src\":\"https://t/large.jpg\"},"
      "\"photographer\":\"Jane Doe\"}]}";
    PsPhoto p = parsePlanespottersPhoto(ok);
    TEST_ASSERT_TRUE(p.ok);
    TEST_ASSERT_EQUAL_STRING("https://t/large.jpg", p.url.c_str());
    TEST_ASSERT_EQUAL_STRING("Jane Doe", p.photographer.c_str());

    // fallback to thumbnail when thumbnail_large absent
    const char* fb =
      "{\"photos\":[{\"thumbnail\":{\"src\":\"https://t/small.jpg\"},"
      "\"photographer\":\"X\"}]}";
    PsPhoto pf = parsePlanespottersPhoto(fb);
    TEST_ASSERT_TRUE(pf.ok);
    TEST_ASSERT_EQUAL_STRING("https://t/small.jpg", pf.url.c_str());
}

void test_parse_planespotters_photo_misses(void) {
    TEST_ASSERT_FALSE(parsePlanespottersPhoto("{\"photos\":[]}").ok);   // no photos
    TEST_ASSERT_FALSE(parsePlanespottersPhoto("{}").ok);                // no key
    TEST_ASSERT_FALSE(parsePlanespottersPhoto("not json").ok);          // malformed
    TEST_ASSERT_FALSE(parsePlanespottersPhoto(
        "{\"photos\":[{\"photographer\":\"X\"}]}").ok);                 // no src at all
}

void test_pick_jpeg_scale_and_crop(void) {
    // largest divisor d in {1,2,4,8} with srcW/d>=240 AND srcH/d>=240, else 1
    TEST_ASSERT_EQUAL(1, pickJpegScale(400, 267));    // 1/2 would undershoot 240
    TEST_ASSERT_EQUAL(2, pickJpegScale(960, 640));
    TEST_ASSERT_EQUAL(4, pickJpegScale(2000, 1500));
    TEST_ASSERT_EQUAL(8, pickJpegScale(4000, 3000));
    TEST_ASSERT_EQUAL(1, pickJpegScale(200, 150));    // undersized -> letterbox
    // centering offsets (can be negative for letterbox)
    TEST_ASSERT_EQUAL(80, cropOffset(400));    // (400-240)/2
    TEST_ASSERT_EQUAL(0, cropOffset(240));
    TEST_ASSERT_EQUAL(-20, cropOffset(200));   // centers an undersized image
}
```

Register: `RUN_TEST(test_parse_planespotters_photo); RUN_TEST(test_parse_planespotters_photo_misses); RUN_TEST(test_pick_jpeg_scale_and_crop);`

- [ ] **Step 2: Run to verify they fail**

Run: `pio test -e native -f test_core`
Expected: compile error — `photo_core.h: No such file or directory`

- [ ] **Step 3: Create `src/photo_core.h`**

```cpp
#pragma once
#include <string>
#include <ArduinoJson.h>

// Planespotters photo metadata + display scale/crop math for the 240x240
// round screen. Arduino-free (ArduinoJson works host-side), host-tested.

struct PsPhoto {
    bool ok = false;
    std::string url;           // thumbnail_large preferred, thumbnail fallback
    std::string photographer;  // attribution (required by planespotters)
};

// Extract photos[0].thumbnail_large.src (fallback thumbnail.src) + photographer.
inline PsPhoto parsePlanespottersPhoto(const std::string& json) {
    PsPhoto r;
    JsonDocument doc;
    if (deserializeJson(doc, json)) return r;
    JsonVariantConst photos = doc["photos"];
    if (!photos.is<JsonArrayConst>() || photos.size() == 0) return r;
    JsonVariantConst p = photos[0];
    const char* src = p["thumbnail_large"]["src"].as<const char*>();
    if (!src) src = p["thumbnail"]["src"].as<const char*>();
    if (!src) return r;
    r.url = src;
    const char* ph = p["photographer"].as<const char*>();
    r.photographer = ph ? ph : "";
    r.ok = true;
    return r;
}

// Largest JPEGDEC divisor d in {1,2,4,8} whose scaled image still covers
// 240x240 in BOTH dimensions; 1 when even full size doesn't (letterbox).
inline int pickJpegScale(int srcW, int srcH) {
    for (int d = 8; d >= 2; d /= 2)
        if (srcW / d >= 240 && srcH / d >= 240) return d;
    return 1;
}

// Centering offset for one scaled dimension onto the 240px target.
// Positive = crop that many source px off the leading edge; negative =
// letterbox margin (image smaller than the screen).
inline int cropOffset(int scaledDim) { return (scaledDim - 240) / 2; }
```

- [ ] **Step 4: Run to verify they pass**

Run: `pio test -e native -f test_core`
Expected: 60/60 PASS

- [ ] **Step 5: Commit**

```bash
git add src/photo_core.h test/test_core/test_main.cpp
git commit -m "feat(firmware): photo_core.h — planespotters parse + jpeg scale/crop math"
```

---

### Task 3: Photo pipeline in the firmware — fetch, decode, PSRAM cache

**Files:**
- Modify: `platformio.ini` (esp32-s3 lib_deps)
- Modify: `src/flight_ticker.ino` (includes, globals after the wifi-scan block ~line 116, helpers after `sendScanResults`)

No host test possible (Arduino/HTTP/JPEGDEC glue); the pure parts were tested in Tasks 1-2. Verification = clean builds.

- [ ] **Step 1: Add the JPEGDEC dependency**

In `platformio.ini` `[env:esp32-s3]` `lib_deps`, add:

```
    bitbank2/JPEGDEC@^1.8.2
```

- [ ] **Step 2: Includes**

In `src/flight_ticker.ino`, next to the other core includes add `#include "photo_core.h"`; next to the library includes (after `<NimBLEDevice.h>`) add `#include <JPEGDEC.h>`.

- [ ] **Step 3: Globals + decode callback**

After the wifi-scan globals/callback block (`WifiScanCallbacks` etc., ~line 116):

```cpp
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
PhotoSlot g_photoCache[PHOTO_CACHE_SLOTS];
std::map<std::string, bool> g_photoMiss;  // negative cache (known no-photo), per boot

JPEGDEC g_jpeg;
// JPEGDEC delivers MCU blocks via callback; these route them into the target
// framebuffer with the centering crop offsets (negative = letterbox margin).
uint16_t* g_decTarget = nullptr;
int g_decOffX = 0, g_decOffY = 0;

int photoDrawCb(JPEGDRAW* d) {
    for (int row = 0; row < d->iHeight; row++) {
        int ty = d->y + row - g_decOffY;
        if (ty < 0 || ty >= 240) continue;
        for (int col = 0; col < d->iWidth; col++) {
            int tx = d->x + col - g_decOffX;
            if (tx < 0 || tx >= 240) continue;
            g_decTarget[ty * 240 + tx] = d->pPixels[row * d->iWidth + col];
        }
    }
    return 1;
}
```

- [ ] **Step 4: HTTP + cache + fetch helpers**

After `sendScanResults` (~line 497):

```cpp
// Download a URL into a PSRAM buffer (caller frees). Same TLS pattern as the
// hexdb lookup; requires Content-Length (planespotters sends it) and caps at
// maxLen. Returns null on any failure.
uint8_t* httpsGetToPsram(const char* url, size_t maxLen, size_t* outLen) {
    WiFiClientSecure client; client.setInsecure();
    HTTPClient http; http.begin(client, url);
    // planespotters 403s generic UAs — must be descriptive with a contact URL
    http.setUserAgent("flight-radar-esp32/1.0 (+https://github.com/disclaimer8/flight-radar-esp32)");
    http.setConnectTimeout(2500); http.setTimeout(4000);
    if (http.GET() != 200) { http.end(); return nullptr; }
    int len = http.getSize();
    if (len <= 0 || (size_t)len > maxLen) { http.end(); return nullptr; }
    uint8_t* buf = (uint8_t*)heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    if (!buf) { http.end(); return nullptr; }
    WiFiClient* s = http.getStreamPtr();
    size_t got = 0;
    unsigned long t0 = millis();
    while (got < (size_t)len && millis() - t0 < 8000) {
        int n = s->read(buf + got, (size_t)len - got);
        if (n > 0) got += (size_t)n; else delay(10);
    }
    http.end();
    if (got != (size_t)len) { heap_caps_free(buf); return nullptr; }
    *outLen = got;
    return buf;
}

// LRU-insert a decoded photo. Eviction can never free the on-screen photo:
// fetches only happen on PHOTO entry, and the entering photo gets the
// freshest lastUse.
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

struct PhotoResult { bool ok = false; uint16_t* px = nullptr; std::string photographer; };

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
    size_t jlen = 0;
    uint8_t* jbuf = httpsGetToPsram(url, 32 * 1024, &jlen);
    if (!jbuf) { g_photoMiss[key] = true; return res; }
    PsPhoto meta = parsePlanespottersPhoto(std::string((char*)jbuf, jlen));
    heap_caps_free(jbuf);
    if (!meta.ok) { g_photoMiss[key] = true; return res; }

    size_t ilen = 0;
    uint8_t* ibuf = httpsGetToPsram(meta.url.c_str(), 150 * 1024, &ilen);
    if (!ibuf) return res;  // transient network failure: don't negative-cache

    uint16_t* px = (uint16_t*)heap_caps_malloc(240 * 240 * 2, MALLOC_CAP_SPIRAM);
    if (!px) { heap_caps_free(ibuf); return res; }
    std::memset(px, 0, 240 * 240 * 2);  // black letterbox margins

    if (g_jpeg.openRAM(ibuf, (int)ilen, photoDrawCb)) {
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
    heap_caps_free(ibuf);
    return res;
}
```

- [ ] **Step 5: Verify both builds**

Run: `pio test -e native -f test_core` → 60/60 (host env untouched — JPEGDEC include is inside the ARDUINO guard)
Run: `pio run -e esp32-s3` → SUCCESS

- [ ] **Step 6: Commit**

```bash
git add platformio.ini src/flight_ticker.ino
git commit -m "feat(firmware): photo fetch/decode pipeline with PSRAM LRU cache (JPEGDEC)"
```

---

### Task 4: PHOTO view — gestures, draw, states

**Files:**
- Modify: `src/flight_ticker.ino` (View enum ~line 49, handleTouch ~line 442, loop tail ~line 636, new functions after `drawDetail`)

- [ ] **Step 1: Extend the view enum and state**

Replace `enum View { RADAR, DETAIL };` with:

```cpp
enum View { RADAR, DETAIL, PHOTO };
```

After `size_t g_idx = 0;` add:

```cpp
uint16_t*   g_photoPx = nullptr;   // cache-owned; valid only in PHOTO view
std::string g_photoCredit;
```

- [ ] **Step 2: Entry + draw functions**

After `drawDetail()` (~line 428):

```cpp
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

// Swipe-up handler in DETAIL: fetch (or cache-hit) the photo and switch view.
// Failure paths flash a message and stay in DETAIL.
void enterPhotoView() {
    if (g_cache.empty()) return;
    if (g_idx >= g_cache.size()) g_idx = 0;
    const Aircraft& ac = g_cache[g_idx];
    if (WiFi.status() != WL_CONNECTED) { flashPhotoMsg("No Wi-Fi"); return; }
    fb.fillSprite(TFT_BLACK);
    fb.setTextDatum(MC_DATUM);
    fb.setTextColor(TFT_CYAN, TFT_BLACK);
    fb.drawString("Loading photo...", CX, CY, 2);
    fb.pushSprite(0, 0);
    PhotoResult r = fetchPhoto(ac);
    if (!r.ok) { flashPhotoMsg("No photo"); return; }
    g_photoPx = r.px;
    g_photoCredit = r.photographer;
    g_view = PHOTO;
}

void drawPhoto() {
    if (!g_photoPx) { g_view = DETAIL; drawDetail(); return; }
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
```

- [ ] **Step 3: Gesture wiring**

In `handleTouch()`, the view branch currently ends with:

```cpp
    } else { // DETAIL
        if (g == TG_LEFT && !g_cache.empty()) {
            g_idx = (g_idx + 1) % g_cache.size();
        } else if (g == TG_RIGHT && !g_cache.empty()) {
            g_idx = (g_idx + g_cache.size() - 1) % g_cache.size();
        } else if (g == TG_CLICK || g == TG_DOWN) {
            g_view = RADAR;
        }
    }
```

Replace with:

```cpp
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
    }
```

- [ ] **Step 4: Idle timeout + render dispatch**

In `loop()`, replace:

```cpp
    if (g_view == DETAIL && now - g_lastTouch >= IDLE_RETURN_MS) g_view = RADAR;
```

with:

```cpp
    if (g_view != RADAR && now - g_lastTouch >= IDLE_RETURN_MS) {
        g_view = RADAR;
        g_photoPx = nullptr;
    }
```

and replace:

```cpp
    if (g_view == RADAR) drawRadar(); else drawDetail();
```

with:

```cpp
    if (g_view == RADAR) drawRadar();
    else if (g_view == DETAIL) drawDetail();
    else drawPhoto();
```

- [ ] **Step 5: Verify both builds**

Run: `pio test -e native -f test_core` → 60/60
Run: `pio run -e esp32-s3` → SUCCESS

- [ ] **Step 6: Commit**

```bash
git add src/flight_ticker.ino
git commit -m "feat(firmware): PHOTO view — swipe up on detail for the aircraft photo"
```

---

### Task 5: Docs + full verification

**Files:**
- Modify: `README.md`, `docs/ARCHITECTURE.md`, `CLAUDE.md`

- [ ] **Step 1: Update docs** (match each file's existing tone; pattern-match the passages describing the detail carousel and range presets):

- `README.md`: detail-view description gains "swipe up for the aircraft's planespotters photo (Wi-Fi only, PSRAM-cached)".
- `docs/ARCHITECTURE.md`: PHOTO view in the view state machine (RADAR⇄DETAIL⇄PHOTO, gestures, idle return); the fetch→decode→cache pipeline (planespotters reg-then-hex, descriptive UA, JPEGDEC scale/crop via photo_core.h, 8-slot PSRAM LRU + negative cache, blocking acceptance); update test counts.
- `CLAUDE.md`: Code layout gains `src/photo_core.h` line; Gotchas gains a photo bullet (planespotters 403s generic UAs; photos Wi-Fi-only; fetch blocks loop 1-3 s; PSRAM cache); update host test count (60) in Toolchain.

- [ ] **Step 2: Full verification**

```bash
pio test -e native -f test_core    # expect 60 cases PASS
pio run -e esp32-s3                # expect SUCCESS
cd companion && flutter test       # expect 53 PASS (app untouched — sanity)
```

- [ ] **Step 3: Commit**

```bash
git add README.md docs/ARCHITECTURE.md CLAUDE.md
git commit -m "docs: PHOTO view — aircraft photos on the device display"
```

- [ ] **Step 4: Hardware smoke (manual, Denys)**

1. Flash; open detail on an airliner with a registration → swipe up → "Loading photo..." → photo with callsign + attribution; colors sane (red/blue swap → flip `setSwapBytes` in `drawPhoto`).
2. Exit (tap), swipe up again → instant (cache hit).
3. GA aircraft without a photo → "No photo", stays in DETAIL; second try instant ("No photo" again — negative cache, no network).
4. BLE mode (Wi-Fi down) → swipe up → "No Wi-Fi".
5. Idle in PHOTO → returns to RADAR; radar smooth after.
6. RAM check over serial: no crash after ~10 photos (LRU eviction working).
