# Aircraft photos over BLE (phone-streamed)

**Date:** 2026-06-15
**Scope:** firmware (`src/`) + Flutter companion (`companion/`)

## Goal

Show aircraft photos in the PHOTO view even when the device is on the **BLE
fallback path** (Wi-Fi down). Over BLE the device has no internet, so the phone —
which does — fetches the photo and streams it to the device, which decodes it with
the existing JPEGDEC pipeline.

**Model: pull.** Swipe-up requests a photo for the selected aircraft; the phone
fetches and streams it back. Mirrors the Wi-Fi UX (fetch only what's viewed; no
wasted BLE bandwidth).

**Format: 240×240 baseline JPEG, extra-compressed.** The phone fetches the same
wsrv.nl re-encoding proxy used on Wi-Fi but with a lower quality
(`&output=jpg&q=55`, tunable) to shrink the BLE transfer. JPEG (~3–8 KB) instead of
raw RGB565 (115 KB) is ~10–30× less to send and reuses the device's decoder.

## Wire protocol — `src/photo_ble_core.h` (pure, host-tested)

All frames travel on one new characteristic (below). Little-endian. Mirrors the
`wifi_scan_core.h` pattern (pure build/parse helpers + a Dart mirror).

Frame magic `'P'` + type:

- **PR** (device → phone, NOTIFY) — "fetch photo":
  `'P','R', reqId(u8), keyLen(u8), key[keyLen]`  (key = registration or hex, ≤11 B)
- **PH** (phone → device, WRITE) — transfer header:
  `'P','H', reqId(u8), totalLen(u32), credLen(u8), credit[credLen]`
  `totalLen == 0` ⇒ no photo available (device shows "No photo").
- **PD** (phone → device, WRITE) — JPEG chunk:
  `'P','D', reqId(u8), seq(u16), bytes[...]`  (bytes appended in `seq` order)

`reqId` is a 1-byte counter the device increments per request; the phone echoes it
in PH/PD. The device drops any PH/PD whose `reqId` ≠ the current request (stale
stream from a previous swipe). Caps: `BLE_PHOTO_MAX = 48 KB` (reject larger
`totalLen`); chunk payload sized to MTU 517 (~500 B/PD → ~6–16 frames typical).

Header file exposes pure helpers: `buildPhotoReq`, `parsePhotoHeader`,
`parsePhotoChunk` (+ a `PhotoBleHeader`/`PhotoBleChunk` struct), all host-tested.

## New GATT characteristic

`BLE_PHOTO_UUID = f1a90005-7e1d-4c2a-9b3f-1a2b3c4d5e6f`, **WRITE + WRITE_NR +
NOTIFY** (one bidirectional characteristic, like the `f1a90004` scan flow). Added
to the existing service in `setup()`.

## Firmware changes

1. **`enterPhotoView()` — BLE branch.** Today it shows "No Wi-Fi" when `!WL_CONNECTED`.
   New: if Wi-Fi is down but BLE is the active source and a central is subscribed,
   send a **PR** notify (new `reqId`, the aircraft key), set the same loading +
   `g_photoReqKey` state used by the Wi-Fi path, and switch to PHOTO. If no BLE
   subscriber, keep the "No Wi-Fi"/"No link" message.
2. **Receive (NimBLE callback).** A `PhotoBleCallbacks::onWrite` buffers PH/PD into
   a dedicated PSRAM buffer `g_blePhotoBuf` (`BLE_PHOTO_MAX`), tracking
   `reqId/expectedLen/got/credit`. The callback only copies bytes + sets flags (no
   decode, same discipline as the other BLE writers). On `got == expectedLen` it
   sets `g_blePhotoReady`.
3. **Decode on netTask via a shared helper.** Extract the decode+cache tail of
   `fetchPhoto()` into `decodeJpegToCache(buf, len, key, credit) -> PhotoResult`
   (JPEGDEC openRAM/decode → fresh PSRAM 240×240 → `photoCacheInsert`; negative-
   cache on undecodable). The Wi-Fi path calls it unchanged; netTask, when
   `g_blePhotoReady` is set, calls it on `g_blePhotoBuf` and publishes through the
   **existing** `g_photoRes*` handoff (reusing the key-identity check + memory
   barrier added in the photo-race fix). `g_jpeg` stays netTask-owned (no new
   concurrency).
4. **Timeout.** When a PR is sent, record a deadline; if no complete transfer
   within `BLE_PHOTO_TIMEOUT_MS` (~8 s), loop clears the loading state → "No photo".
   Safe: loop owns the BLE-photo loading/deadline state.

## Companion (Flutter) changes

1. **`ble_manager`** subscribes to `f1a90005` notifications.
2. On a **PR** notify: parse `reqId` + key. Use `photo_client` to fetch the
   wsrv.nl-proxied, extra-compressed 240×240 baseline JPEG for the key (new
   `buildProxiedPhotoUrl(src, q)` — Dart mirror of `photo_core.h`, with `&q=55`).
   Then write **PH** (`reqId`, length, photographer) followed by **PD** chunks
   (`reqId`, `seq`, bytes) using write-with-response (flow control). No photo /
   fetch error ⇒ PH with `totalLen=0`.
3. **`packet/photo_ble_packet.dart`** — Dart build/parse of PR/PH/PD + a chunker,
   with Flutter unit tests (mirror of the C++ host tests).

## Error handling

- Identity: device applies a decoded photo only when the result key matches the
  view's `g_photoReqKey` (existing check) and the stream's `reqId` matches.
- `totalLen == 0`, oversized `totalLen`, malformed frame, or device timeout →
  "No photo" (no crash, no lock-up). On-device negative cache suppresses repeats.
- Attribution: planespotters credit travels in PH and renders as today
  (`(c) <photographer> / planespotters.net`).

## Testing

- **Host** (`pio test -e native`): `photo_ble_core.h` round-trips and rejects
  (bad magic, truncation, `totalLen=0`, oversize, reqId mismatch).
- **Flutter** (`flutter test`): `photo_ble_packet` build/parse + chunker + proxied
  URL builder.
- **On device**: force BLE mode (Wi-Fi off), swipe up → photo appears within a few
  seconds; wrong/stale swipes never show the wrong photo; missing photo shows
  "No photo" and recovers.

## Out of scope (YAGNI)

- Push/prefetch of photos (pull only).
- Photo transfer while Wi-Fi is up (Wi-Fi path unchanged).
- Changes to the v3 aircraft packet, Wi-Fi provisioning, or scan characteristics.
