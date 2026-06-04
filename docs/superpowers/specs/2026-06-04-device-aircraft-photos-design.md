# Aircraft photos on the device (round GC9A01 display)

Add a PHOTO view to the firmware's detail flow: swipe up on an aircraft's
detail page to see its planespotters photo on the round display.

## UX / state machine

Current views: `RADAR ⇄ DETAIL` (`flight_ticker.ino:49`). In DETAIL,
left/right cycles aircraft, click/down returns to radar, **up is unused** —
it becomes the photo gesture.

- DETAIL + swipe up → new `PHOTO` view for the current aircraft.
- PHOTO: shows the photo center-cropped to 240×240 (round bezel clips the
  corners — acceptable for photos) with two small overlays: callsign
  (top-center) and `© photographer / planespotters.net` attribution
  (bottom-center, required by planespotters).
- PHOTO + any touch (click/up/down/left/right) → back to DETAIL.
  (v1 keeps it simple: no aircraft cycling inside PHOTO.)
- The existing idle timeout (`IDLE_RETURN_MS`) also returns PHOTO → RADAR.
- While fetching: black screen with "Loading photo…"; on failure or no photo:
  "No photo" for ~1.5 s, then auto-return to DETAIL.
- BLE-fallback mode (no Wi-Fi): swipe up shows "No Wi-Fi" briefly and stays
  in DETAIL. Photos are Wi-Fi-only — pushing images over BLE is out of scope
  (492 B/write makes a 30 KB JPEG take ~a minute).

## Data pipeline (firmware)

1. **Lookup**: planespotters public API, registration first then hex —
   `https://api.planespotters.net/pub/photos/reg/{reg}` /
   `.../hex/{hex}` — same order as the companion app. MUST send the
   descriptive User-Agent (`flight-radar-esp32/1.0 (+repo URL)`) — generic
   UAs get HTTP 403 (trap already hit in the app). Parse `photos[0]
   .thumbnail_large.src` (fallback `thumbnail.src`) + `photographer`.
2. **Fetch**: HTTPS GET of the JPEG (~400 px wide, 20–60 KB) into a PSRAM
   download buffer (cap 150 KB; abort beyond). Same `WiFiClientSecure` +
   `setInsecure()` + bounded timeouts (2.5 s connect) pattern as the hexdb
   route lookup.
3. **Decode**: `bitbank2/JPEGDEC` (new PlatformIO dep) — streams MCU blocks,
   minimal RAM. Decode at the scale (1/1, 1/2, 1/4, 1/8) that best
   center-crop-covers 240×240, render via pixel callback into a PSRAM
   framebuffer (240×240×16bit = 115 KB), then push to the TFT.
4. **Blocking is accepted**: lookup + fetch + decode run synchronously in
   `loop()` (~1–3 s), like the existing provisioning (~12 s) and poll (~8 s)
   blocks — a deliberate one-shot user action. Radar freshness is untouched.

## Caching

- Decoded-photo cache in PSRAM keyed by registration (or hex when reg is
  empty): N×115 KB slots, LRU, N=8 (~920 KB of the 2 MB PSRAM; download
  buffer + headroom in the rest). Cache hits render instantly.
- Negative cache (no-photo results) in a small map, like the app's
  `PhotoClient` — never re-hit the API for a known miss within a boot.
- RAM-only; cleared on reboot (photos are ephemeral, flash wear not worth it).

## Code layout

- `src/photo_core.h` (NEW, Arduino-free, host-tested): planespotters JSON
  extraction (`parsePlanespottersPhoto` → {url, photographer} | none) +
  center-crop/scale math (`pickJpegScale(srcW, srcH)` → scale + crop offsets).
- `src/flight_ticker.ino`: `PHOTO` view enum + gesture wiring + `drawPhoto()`
  + fetch/decode/cache glue (PSRAM via `heap_caps_malloc(MALLOC_CAP_SPIRAM)`).
- `platformio.ini`: add `bitbank2/JPEGDEC` dep (pin current major).
- Companion app: NO changes.

## Memory budget (S3, 2 MB PSRAM, BOARD_HAS_PSRAM already set)

- SRAM: unchanged (115 KB sprite + TLS + NimBLE stay as-is; JPEGDEC works in
  ~20 KB SRAM internally).
- PSRAM: 8×115 KB cache + ≤150 KB download buffer ≈ 1.07 MB of 2 MB. Headroom
  retained.

## Testing

- Host (`pio test -e native`): `parsePlanespottersPhoto` (happy path, no
  photos, missing thumbnail_large → fallback, malformed JSON),
  `pickJpegScale` rule: largest divisor d ∈ {1,2,4,8} with srcW/d ≥ 240 AND
  srcH/d ≥ 240, else d=1 (undersized images upscale-free, letterboxed).
  Cases: 400×267 → 1 (1/2 would undershoot 240), 960×640 → 2, 2000×1500 → 4,
  200×150 → 1; crop offsets center the result.
- Firmware build: `pio run -e esp32-s3`.
- Hardware smoke (manual): photo loads on swipe-up, cache hit instant on
  re-entry, "No photo" path (GA aircraft without photos), BLE-mode "No Wi-Fi"
  path, attribution legible, radar resumes cleanly.

## Out of scope

- BLE photo push from the phone.
- Flash/LittleFS persistent cache.
- Prefetching photos for all visible aircraft.
- Aircraft cycling inside the PHOTO view (left/right) — v2 if wanted.
