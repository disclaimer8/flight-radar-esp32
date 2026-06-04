# Network Offload to Core 1 + Persistent TLS Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The radar render loop never blocks on the network: poll, route, and photo HTTP move to a dedicated FreeRTOS task on core 0, with a persistent keep-alive TLS client for the poll.

**Architecture:** `netTask` (pinned core 0; Arduino `loopTask` renders on core 1) owns all outbound HTTP. Hand-off extends the codebase's native "producer fills buffer + sets volatile flag, loop() applies at one safe point" pattern (see `g_bleBuf`/`g_blePacketReady`) with per-channel double buffers. Caches (`g_routeCache`, photo cache, JPEGDEC, download scratch) become netTask-private; cross-core payloads are fixed char arrays or swapped vectors, never live std::string/map references.

**Tech Stack:** ESP32-S3 Arduino, FreeRTOS `xTaskCreatePinnedToCore`, existing HTTPClient/WiFiClientSecure with `setReuse(true)`.

**Spec:** `docs/superpowers/specs/2026-06-04-network-task-offload-design.md`

**Cross-core memory note (house convention):** internal SRAM on ESP32 is uncached and stores complete in order per core; the existing volatile-flag handoff (NimBLE task → loop) relies on this. Every channel below follows single-writer-per-field + flag-written-last discipline. No new synchronization primitives.

**All tasks modify ONLY `src/flight_ticker.ino`** (Task 4 adds docs). Verification each task: `pio test -e native -f test_core` (61) + `pio run -e esp32-s3` SUCCESS. Hardware smoke at the end. Branch: `feature/perf-pass`.

---

### Task 1: netTask skeleton + poll channel + persistent TLS

**Files:** Modify `src/flight_ticker.ino`.

- [ ] **Step 1: Channel globals + task body.** After the photo-pipeline globals block (after `g_photoDlBuf`), insert:

```cpp
// ---- netTask: all outbound HTTP on core 0 (render loop stays on core 1) ----
// Hand-off = the house volatile-flag pattern: netTask is the only writer of
// each result buffer, sets the ready flag LAST; loop() consumes at one safe
// point and clears the flag. Internal SRAM is uncached and per-core stores
// land in order, same assumption the BLE->loop path has always made.

// Poll channel: netTask parses into its scratch, then swap-publishes here.
std::vector<Aircraft> g_pollBuf;
volatile bool g_pollReady = false;

void netPoll() {
    // Persistent keep-alive client: handshake once (~1.5 s), then each poll
    // is a bare GET (~200 ms). On any failure tear down so the next cycle
    // re-handshakes from scratch.
    static WiFiClientSecure s_client;
    static HTTPClient s_http;
    static bool s_init = false;
    if (!s_init) {
        s_client.setInsecure();   // public read-only data; no CA pinning
        s_http.setReuse(true);
        s_init = true;
    }
    char url[160];
    std::snprintf(url, sizeof(url),
        "https://api.airplanes.live/v2/point/%.4f/%.4f/%d",
        g_obsLat, g_obsLon, queryRadiusNm(kRangePresets[kRangeCount - 1]));
    s_http.begin(s_client, url);
    s_http.setUserAgent("flight-ticker-esp32");
    s_http.setConnectTimeout(8000);
    s_http.setTimeout(8000);
    int code = s_http.GET();
    if (code == 200) {
        String payload = s_http.getString();
        s_http.end();   // setReuse keeps the socket alive
        std::vector<Aircraft> fresh = parseNearest(
            std::string(payload.c_str()), g_obsLat, g_obsLon,
            RADAR_PLOT_CAP, HIDE_GROUND_AIRCRAFT);
        Serial.printf("poll ok: %u aircraft\n", (unsigned)fresh.size());
        if (!g_pollReady) {            // loop consumed the previous batch
            g_pollBuf.swap(fresh);
            g_pollReady = true;        // flag last
        }
    } else {
        Serial.printf("poll failed: %d\n", code);
        s_http.end();
        s_client.stop();               // force a clean handshake next cycle
        g_stale = true;                // single-writer note: see Step 3
    }
}

void netTask(void*) {
    unsigned long lastPoll = 0;
    for (;;) {
        if (WiFi.status() == WL_CONNECTED &&
            millis() - lastPoll >= POLL_INTERVAL_MS) {
            lastPoll = millis();
            netPoll();
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
```

(`g_stale` write from netTask: bool, single writer for the `true` transition; loop writes `false` only inside the consume block below — no torn state possible on a bool. Keep the comment.)

- [ ] **Step 2: Delete the old `pollApi()`** (lines ~286–315: the whole function including its `else { g_stale = true; ... }` tail — read it first and make sure every piece of its bookkeeping reappears either in `netPoll` (fetch/parse/stale-on-fail) or the loop consume (Step 3: idx clamp, center, stale clear, `g_lastPoll`)).

- [ ] **Step 3: Loop-side consume + remove the blocking call.** In `loop()`, right after the `g_blePacketReady` apply block, insert:

```cpp
    if (g_pollReady) {
        g_cache.swap(g_pollBuf);
        g_pollBuf.clear();
        g_pollReady = false;           // netTask may publish again now
        if (g_idx >= g_cache.size()) g_idx = 0;
        g_centerLat = g_obsLat; g_centerLon = g_obsLon;
        g_stale = false;
        g_lastPoll = millis();         // freshness = when applied
    }
```

Replace the old poll block (find this exact code):

```cpp
    // pollApi() blocks up to ~8s (HTTP timeout); the sweep freezes and touches are
    // ignored for that window. Acceptable on this single-threaded firmware — not a bug.
    if (now - g_lastPoll >= POLL_INTERVAL_MS) {
        if (WiFi.status() == WL_CONNECTED) pollApi();   // skip when offline; no blocking reconnect
        g_lastPoll = now;
    }
```

with:

```cpp
    // Polling lives on netTask (core 0); g_lastPoll is bumped at consume time
    // above and only feeds the staleness indicator now.
```

- [ ] **Step 4: setup() changes.** Find `pollApi();` in `setup()` (after `connectWifi();`) and replace it + the line `g_lastPoll  = millis();` with:

```cpp
    // First poll comes from netTask within ~2 s of boot.
    xTaskCreatePinnedToCore(netTask, "net", 12288, nullptr, 1, nullptr, 0);
    g_lastPoll = millis();
```

(12 KB stack: TLS handshake + filtered JSON parse. Smoke test logs the high-water mark — Task 4.)

- [ ] **Step 5: Verify** `pio test -e native -f test_core` → 61/61; `pio run -e esp32-s3` → SUCCESS.

- [ ] **Step 6: Commit** `git add src/flight_ticker.ino && git commit -m "perf(firmware): poll moves to netTask on core 0 with persistent keep-alive TLS"`

---

### Task 2: route channel — DETAIL never blocks

**Files:** Modify `src/flight_ticker.ino`.

- [ ] **Step 1: Mailbox globals.** After the poll-channel globals:

```cpp
// Route channel: loop posts a callsign, netTask resolves (its private cache /
// hexdb) and publishes. Fixed char buffers — std::string must not cross cores.
char          g_routeReqKey[12] = "";
volatile bool g_routeReq = false;      // loop sets, netTask clears
char          g_routeResKey[12] = "";  // written LAST by netTask
char          g_routeResOrigin[8] = "";
char          g_routeResDest[8] = "";
```

- [ ] **Step 2: netTask handler.** In `netTask`'s for-loop, after the poll branch:

```cpp
        if (g_routeReq) {
            char key[12];
            strlcpy(key, (const char*)g_routeReqKey, sizeof(key));
            auto rt = lookupRoute(key);                     // netTask-private cache
            strlcpy(g_routeResOrigin, rt.first.c_str(), sizeof(g_routeResOrigin));
            strlcpy(g_routeResDest, rt.second.c_str(), sizeof(g_routeResDest));
            strlcpy(g_routeResKey, key, sizeof(g_routeResKey));   // key last = result complete
            g_routeReq = false;
        }
```

Add a comment on `g_routeCache`/`lookupRoute` (they stay where they are): `// netTask-PRIVATE from here on: loop() must never call lookupRoute or read g_routeCache (std::map across cores = UB).`

- [ ] **Step 3: drawDetail goes non-blocking.** Replace (find exact):

```cpp
    std::string rOrigin = ac.origin, rDest = ac.dest;
    if (rOrigin.empty() && WiFi.status() == WL_CONNECTED) {
        auto rt = lookupRoute(ac.callsign);
        rOrigin = rt.first; rDest = rt.second;
    }
```

with:

```cpp
    // Route comes from the netTask mailbox — never block the render loop on
    // hexdb. Until the result lands, the row shows a "..." placeholder.
    std::string rOrigin = ac.origin, rDest = ac.dest;
    bool routePending = false;
    if (rOrigin.empty() && WiFi.status() == WL_CONNECTED && !ac.callsign.empty()) {
        if (ac.callsign == (const char*)g_routeResKey) {
            rOrigin = (const char*)g_routeResOrigin;
            rDest   = (const char*)g_routeResDest;
        } else if (!g_routeReq) {
            strlcpy(g_routeReqKey, ac.callsign.c_str(), sizeof(g_routeReqKey));
            g_routeReq = true;         // flag last
            routePending = true;
        } else {
            routePending = true;       // a request (this or another) is in flight
        }
    }
```

and where the route row is drawn (find exact):

```cpp
    if (!rOrigin.empty() && rOrigin != rDest)
        fb.drawString((rOrigin + " > " + rDest).c_str(), CX, 186, 2);
```

replace with:

```cpp
    if (!rOrigin.empty() && rOrigin != rDest)
        fb.drawString((rOrigin + " > " + rDest).c_str(), CX, 186, 2);
    else if (routePending)
        fb.drawString("...", CX, 186, 2);
```

- [ ] **Step 4: Verify** both builds. **Step 5: Commit** `"perf(firmware): non-blocking route lookups via netTask mailbox (DETAIL opens instantly)"`

---

### Task 3: photo channel — non-blocking, cancellable PHOTO loading

**Files:** Modify `src/flight_ticker.ino`.

- [ ] **Step 1: Mailbox globals.** After the route-channel globals:

```cpp
// Photo channel: loop posts reg/hex, netTask runs the fetch/decode pipeline
// (its private cache) and publishes the cache-owned pixel pointer + credit.
char          g_photoReqReg[12] = "";
char          g_photoReqHex[8] = "";
volatile bool g_photoReq = false;      // loop sets, netTask clears
uint16_t*     g_photoResPx = nullptr;
char          g_photoResCredit[48] = "";
volatile bool g_photoResOk = false;
volatile bool g_photoReady = false;    // written LAST by netTask
```

and view-state globals next to `g_photoPx`:

```cpp
bool          g_photoLoading = false;          // PHOTO view: request in flight
unsigned long g_photoMsgUntil = 0;             // non-blocking "No photo" deadline
```

- [ ] **Step 2: netTask handler** after the route branch:

```cpp
        if (g_photoReq) {
            Aircraft ac;
            ac.registration = (const char*)g_photoReqReg;
            ac.hex          = (const char*)g_photoReqHex;
            PhotoResult r = fetchPhoto(ac);    // netTask-private cache/decoder
            g_photoResPx = r.px;
            strlcpy(g_photoResCredit, r.photographer.c_str(), sizeof(g_photoResCredit));
            g_photoResOk = r.ok;
            g_photoReady = true;               // flag last
            g_photoReq = false;
        }
```

Mark `fetchPhoto`, `g_photoCache`, `g_photoMiss`, `g_jpeg`, `g_photoDlBuf`, `httpsGetToBuf` with a block comment: netTask-private from here on.

**Eviction invariant (add as a comment near photoCacheInsert, and the reviewer must verify it):** inserts/evictions happen only while a request is outstanding; while a request is outstanding the loop displays "Loading" (g_photoPx == nullptr); therefore eviction can never free a displayed photo.

- [ ] **Step 3: enterPhotoView becomes a post + view switch.** Replace its body after the Wi-Fi guard (keep guards + "No Wi-Fi" flash + drainTouch as-is) — delete the "Loading photo..." draw, the `fetchPhoto` call, the failure flash, and the success assignment; instead:

```cpp
    strlcpy(g_photoReqReg, ac.registration.c_str(), sizeof(g_photoReqReg));
    strlcpy(g_photoReqHex, ac.hex.c_str(), sizeof(g_photoReqHex));
    g_photoReq = true;
    g_photoLoading = true;
    g_photoPx = nullptr;
    g_photoMsgUntil = 0;
    g_view = PHOTO;
```

(`drainTouch()` before `g_view = PHOTO` is no longer needed — nothing blocks anymore; remove the drain from the success path but KEEP it after the "No Wi-Fi" flash, which still blocks 1.2 s.)

- [ ] **Step 4: loop-side consume.** After the poll consume block:

```cpp
    if (g_photoReady) {
        g_photoReady = false;
        if (g_view == PHOTO && g_photoLoading) {
            g_photoLoading = false;
            if (g_photoResOk) {
                g_photoPx = g_photoResPx;
                g_photoCredit = (const char*)g_photoResCredit;
            } else {
                g_photoMsgUntil = millis() + 1500;   // "No photo", then back
            }
        }
        // else: user already left PHOTO — discard the late result.
    }
```

- [ ] **Step 5: drawPhoto handles the two transient states.** At the top of `drawPhoto`, replace `if (!g_photoPx) { g_view = DETAIL; drawDetail(); return; }` with:

```cpp
    if (!g_photoPx) {
        if (g_photoMsgUntil != 0) {                 // failed: flash then return
            if (millis() >= g_photoMsgUntil) {
                g_photoMsgUntil = 0;
                g_view = DETAIL; drawDetail(); return;
            }
            fb.fillSprite(TFT_BLACK);
            fb.setTextDatum(MC_DATUM);
            fb.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
            fb.drawString("No photo", CX, CY, 2);
            fb.pushSprite(0, 0);
            return;
        }
        if (g_photoLoading) {                       // request in flight
            fb.fillSprite(TFT_BLACK);
            fb.setTextDatum(MC_DATUM);
            fb.setTextColor(TFT_CYAN, TFT_BLACK);
            fb.drawString("Loading photo...", CX, CY, 2);
            fb.pushSprite(0, 0);
            return;
        }
        g_view = DETAIL; drawDetail(); return;      // shouldn't happen; safe out
    }
```

- [ ] **Step 6: touch exit cancels loading.** In `handleTouch`'s PHOTO branch add the state resets:

```cpp
    } else { // PHOTO: any touch returns to the detail page
        g_view = DETAIL;
        g_photoPx = nullptr;   // cache still owns the pixels
        g_photoLoading = false;   // a late netTask result will be discarded
        g_photoMsgUntil = 0;
    }
```

Same two resets in the idle-return block in `loop()`.

- [ ] **Step 7: Verify** both builds. **Step 8: Commit** `"perf(firmware): non-blocking photo loading via netTask (cancellable, render never freezes)"`

---

### Task 4: docs + verification + hardware smoke prep

- [ ] **Step 1: Docs.** `docs/ARCHITECTURE.md`: new netTask section (channels, single-writer/flag-last contract, netTask-private caches, persistent TLS, core pinning); update the "pollApi blocks the sweep" passages (now stale) and the photo "blocking 1-3 s" passages. `CLAUDE.md`: update the Gotchas bullets that describe blocking poll/photo + add the netTask contract line ("network on core 0; loop() is the only writer of g_cache; route/photo via mailboxes; never call lookupRoute/fetchPhoto from loop()").

- [ ] **Step 2: Add a one-shot stack watermark log.** In `netTask` after the first successful poll: `Serial.printf("net stack high-water: %u\n", (unsigned)uxTaskGetStackHighWaterMark(NULL));` (keep it — one line per boot is fine).

- [ ] **Step 3: Verify** `pio test -e native -f test_core` 61/61, `pio run -e esp32-s3` SUCCESS, `cd companion && flutter test` 53/53 (untouched — sanity).

- [ ] **Step 4: Commit** `"docs: netTask architecture (core-0 network offload)"`

- [ ] **Step 5: Hardware smoke (manual, после прошивки):**
1. Sweep stays glassy across ≥3 poll cycles (watch the staleness dot stay green).
2. DETAIL opens instantly on a Wi-Fi aircraft without BLE route → "..." → route fills ≤2 s.
3. PHOTO: fresh aircraft → Loading (sweep-independent) → photo; tap DURING loading → instant DETAIL, no glitch when the late result lands; aircraft without photo → "No photo" 1.5 s → DETAIL.
4. BLE provisioning + Wi-Fi scan from the app still work (loop-side, untouched).
5. 30 min soak: no reboot, photos still load (PSRAM/heap stable), serial shows the stack watermark ≥ ~2 KB headroom.
