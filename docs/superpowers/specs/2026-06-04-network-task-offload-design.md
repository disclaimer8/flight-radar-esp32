# Network offload to core 1 + persistent TLS (firmware perf, package B)

Eliminate render-loop freezes caused by blocking network: today `pollApi`
(~1–2 s every 15 s), `lookupRoute` (~1–2 s on first DETAIL open), and the
photo fetch (1–3 s) all run inside `loop()` and stall the radar sweep.

## Architecture

A dedicated FreeRTOS task (`netTask`, pinned to core 0; Arduino `loopTask`
runs on core 1) owns ALL outbound HTTP. The render loop never blocks on the
network again. Communication reuses the codebase's existing no-race pattern —
producer fills a buffer + sets a flag, `loop()` applies at a safe point —
extended with double-buffering for the multi-KB payloads:

- **Requests** (loop → netTask): a small command mailbox — `volatile` request
  flags + parameter slots (poll is timer-driven inside netTask itself; route
  and photo are one-shot requests carrying callsign / aircraft key).
- **Results** (netTask → loop): per-channel double buffer + `volatile bool`
  ready-flag, exactly like `g_bleBuf`/`g_blePacketReady` today.

### Channels

1. **Poll**: netTask runs the 15 s poll timer, fetches airplanes.live,
   `parseNearest` into a scratch `std::vector<Aircraft>`, publishes via
   swap-buffer + flag. `loop()` swaps it into `g_cache` at the top of the
   frame (same place BLE packets apply). The Wi-Fi/BLE source arbitration
   logic stays in `loop()` and is unchanged.
2. **Route**: DETAIL render no longer calls `lookupRoute` synchronously.
   `drawDetail` reads only the route cache; on a miss it shows `"..."`
   (placeholder) and posts a route request; netTask resolves + stores into
   the cache; the next frame picks it up (cache map guarded — see Safety).
3. **Photo**: `enterPhotoView` posts a photo request and switches to a
   non-blocking "Loading photo..." state inside the PHOTO view; netTask runs
   the existing `fetchPhoto` pipeline (lookup → wsrv fetch → decode → cache
   insert) and publishes the result; the PHOTO view shows the photo / "No
   photo" when the flag lands. Touch stays responsive during loading (any
   touch cancels back to DETAIL; a late result is ignored).
4. **Wi-Fi scan + provisioning**: UNCHANGED (BLE-triggered, already
   flag-based; provisioning's deliberate 12 s block is acceptable and rare;
   moving them is out of scope).

### Persistent TLS / keep-alive (poll channel only)

netTask owns a long-lived `WiFiClientSecure` + `HTTPClient` for
api.airplanes.live with `setReuse(true)`: handshake once, then keep-alive
GETs (~200 ms instead of ~1.5 s). On any failure: close, reconnect, retry
once next cycle. Route (hexdb), photo (planespotters + wsrv) keep per-request
clients — different hosts, low frequency, not worth pooling.

## Safety rules (these are the review gates)

- `g_cache` is ONLY written by `loop()` (the swap point). netTask never
  touches it — it publishes into `g_pollBuf` (its own vector) + flag.
- `g_routeCache` (std::map) becomes netTask-PRIVATE — a map read from two
  cores while the other inserts is UB. `drawDetail` stops reading the map;
  instead a single-slot "resolved route" mailbox (key + origin + dest +
  ready flag, double-buffered like the rest) carries the current aircraft's
  route from netTask to the renderer.
- Photo cache (`g_photoCache` slots, PSRAM px buffers): netTask-private for
  inserts; `loop()` receives the `PhotoResult` (pointer into the cache) via
  mailbox. Eviction can no longer race the displayed photo: netTask only
  evicts during an insert, inserts only happen for the requested key, and the
  request is only outstanding while PHOTO view shows "Loading" (not a cached
  photo). Invariant preserved; must be re-verified in review.
- JPEGDEC `g_jpeg` + scratch buffer: used only by netTask now (single owner).
- No `String`/heap object crosses cores by reference; publish by value or by
  swap of dedicated buffers.
- Stack: 12 KB for netTask (TLS + ArduinoJson filter parse); verify with
  `uxTaskGetStackHighWaterMark` during smoke and note the number.
- Single-radio caveat unchanged: Wi-Fi scan (BLE-triggered, loop-side) can
  still stall netTask's in-flight HTTP — acceptable, documented.

## What the user sees

- Radar sweep never hitches: poll happens invisibly.
- DETAIL opens instantly; route shows "..." then fills in ≤2 s.
- PHOTO: "Loading photo..." is now interruptible (any touch returns to
  DETAIL immediately); photos appear without freezing anything else.
- Poll data is fresher (keep-alive: poll completes in ~200 ms).

## Testing

- Host: pure logic stays in headers (no change); any new pure helpers (e.g.
  mailbox structs) get host tests only if they contain logic (likely not —
  they're plain structs; don't force it).
- Firmware build + the standard suites stay green.
- Hardware smoke (the real gate): sweep smoothness during polls (watch ≥3
  poll cycles), DETAIL route placeholder fill, photo load + cancel-during-
  load, BLE provisioning still works (loop-side), Wi-Fi scan during poll,
  30+ min soak for heap/stack stability.

## Out of scope

- Moving provisioning/scan to netTask.
- DMA double-buffer rendering (transfer-bound problem solved by 80 MHz SPI).
- mbedTLS session resumption (keep-alive is the simpler first lever; only if
  keep-alive proves flaky against Cloudflare).
