# BLE Fallback (firmware) — Design

**Date:** 2026-06-01
**Status:** Approved (brainstorming), ready for implementation plan
**Scope:** Sub-project A of the "phone companion" feature — firmware BLE receiver
+ Wi-Fi/BLE source arbitration + the wire protocol. The mobile companion app is
**sub-project B**, a separate later spec that implements against this protocol.

## Summary

Add a Bluetooth Low Energy fallback data path to the Flight Radar firmware. A
phone companion app (sub-project B) acts as an internet gateway: it pulls nearby
aircraft from airplanes.live over cellular and writes them to the device over
BLE. When Wi-Fi is unavailable, the device renders its radar from the
BLE-supplied data, centered on the phone's GPS position. Wi-Fi stays the primary
source whenever it is connected.

## Model

- **Wi-Fi primary.** When connected, the device polls airplanes.live as today,
  centered on the configured `MY_LAT/MY_LON`.
- **BLE fallback.** When Wi-Fi is not connected, the device renders from the most
  recent BLE packet (if fresh), centered on the phone-supplied GPS coordinates.
- **Phone = gateway.** The phone computes nothing about bearing/distance; it
  sends its GPS center + a list of aircraft. The device's existing `render_core`
  computes bearing/distance/compass exactly as in the Wi-Fi path.

## Wire protocol

A single fixed-layout binary packet, little-endian, written to one GATT
characteristic in a single BLE write (sized to fit one ATT MTU; no chunking).

**Header (12 bytes):**

| Field | Type | Notes |
|-------|------|-------|
| magic | u8[2] | `'F','R'` (0x46 0x52) — rejects stray writes |
| version | u8 | `1` |
| count | u8 | number of records, 0..`BLE_MAX_AIRCRAFT` (16) |
| center_lat | f32 | phone GPS latitude (degrees) |
| center_lon | f32 | phone GPS longitude (degrees) |

**Record (28 bytes each), repeated `count` times:**

| Field | Type | Notes |
|-------|------|-------|
| callsign | u8[8] | ASCII, space-padded, trimmed on parse |
| type | u8[4] | ASCII, space-padded, trimmed |
| lat | f32 | degrees |
| lon | f32 | degrees |
| alt_ft | i32 | barometric altitude, feet |
| gs_kt | i16 | ground speed, knots |
| flags | u8 | bit0 onGround, bit1 altValid, bit2 gsValid |
| reserved | u8 | 0 (alignment / future use) |

Max packet = 12 + 16×28 = **460 bytes**, within a 512-byte negotiated MTU
(usable ATT payload ≈ MTU − 3). `count` over 16 or a length that does not equal
`12 + count×28` is rejected.

Decoding into `Aircraft` (from `flight_core.h`): `onGround` from bit0; `altFt` =
NaN if bit1 clear else `alt_ft`; `gsKt` = NaN if bit2 clear else `gs_kt`;
`lat/lon` as sent; `distKm` is left for the device to compute against the center.

## Components

### `src/ble_core.h` — new, pure, Arduino-free, host-tested

The wire parser, mirroring the `render_core.h`/`flight_core.h` pattern (inline
functions, no Arduino includes, unit-tested under `[env:native]`).

```
struct BlePacket {
    bool   ok = false;
    double centerLat = 0, centerLon = 0;
    std::vector<Aircraft> aircraft;   // distKm unset; caller fills via haversine
};
BlePacket parseBlePacket(const uint8_t* buf, size_t len);
```

- Validates magic, version, and that `len == 12 + count*28` with `count <= 16`.
- Decodes header + records into `Aircraft` per the table above.
- Returns `ok=false` (empty) on any validation failure — never partial.
- The caller computes `distKm` (and the renderer computes bearing) against
  `centerLat/centerLon`, reusing `haversineKm`/`bearingDeg`.

Constants (`BLE_MAGIC0/1`, `BLE_VERSION`, `BLE_MAX_AIRCRAFT=16`, record/header
sizes) live here so the app spec can reference one source of truth.

### `src/flight_ticker.ino` — additions

- **NimBLE peripheral.** Advertise a named service (`"FlightRadar"`) with one
  **ingest** characteristic (write / write-no-response). Always advertising, so
  the phone can connect anytime. The write callback copies the raw bytes into a
  static buffer (≤512) and sets a `volatile g_blePacketReady` flag + length; it
  does **not** parse or touch `g_cache` (keeps the BLE-task callback short and
  avoids races with the render loop).
- **Packet handling in `loop()`.** When `g_blePacketReady`, call
  `parseBlePacket`; on `ok`, fill each aircraft's `distKm` via `haversineKm`
  against the packet center, sort nearest-first, cap to `MAX_AIRCRAFT`, store in
  `g_cache`, set `g_centerLat/g_centerLon` to the packet center, and stamp
  `g_bleLastRx = millis()`.
- **Radar center is now a variable.** Introduce `g_centerLat/g_centerLon`
  (doubles). Wi-Fi path sets them to `MY_LAT/MY_LON`; BLE path sets them from the
  packet. `drawRadar`/`drawDetail` use `g_center*` instead of `MY_LAT/MY_LON`.
- **Source arbitration** (evaluated on each poll tick + before render):
  - `SRC_WIFI` if `WiFi.status()==WL_CONNECTED` (poll API as today, center =
    config).
  - else `SRC_BLE` if `millis() - g_bleLastRx <= BLE_FRESHNESS_MS` (30 s).
  - else `SRC_NONE` (render last frame + stale marker).
  - Wi-Fi wins when both are available.
- **Source indicator.** Draw a small `W` (Wi-Fi) or `B` (BLE) glyph near the
  existing stale dot, so the active source is visible at a glance.

### `platformio.ini`

- Add `lib_deps`: `h2zero/NimBLE-Arduino` (pinned in the plan).
- No new build flags expected beyond NimBLE defaults.

### `src/config.example.h` / `config.h`

- Add `#define BLE_FRESHNESS_MS 30000`. (BLE device name / UUIDs are firmware
  constants, not user config.)

## Error handling / risks

- **Bad/truncated/foreign packet** → `parseBlePacket` returns `ok=false`; cache
  untouched. The `magic`+`version`+exact-length checks reject noise.
- **Phone disconnects / data goes stale** → after `BLE_FRESHNESS_MS`, source
  drops to `SRC_NONE`; the radar shows the last frame with the stale marker.
- **RAM pressure** — the 115 KB 16-bit sprite + Wi-Fi/TLS + NimBLE may strain
  internal RAM. Mitigation: the documented 8-bit sprite fallback
  (`fb.setColorDepth(8)`, ~58 KB). NimBLE (not Bluedroid) is chosen for lower
  RAM/flash. Flash is ~28% used — room for the stack. The plan must verify a
  clean build + on-device boot with BLE active and check free heap.
- **Radio coexistence** — Wi-Fi STA + BLE share the single radio (time-sliced).
  BLE here is low-duty (occasional writes), so coexistence is acceptable; the
  device does not need both at full throughput simultaneously.
- **Security (v1)** — open GATT write, no bonding. Acceptable for a short-range
  hobby device; the packet magic/version reject garbage. Pairing/auth is a
  possible later addition, explicitly out of scope here.

## Testing

- **Host (`[env:native]`, Unity):** new `test_ble` (or extend `test_core`) for
  `parseBlePacket`: a valid packet decodes to the right center + aircraft;
  rejects bad magic, wrong version, `count>16`, and `len != 12+count*28`;
  on-ground/alt-invalid/gs-invalid flags map to the right `Aircraft` fields;
  callsign/type trimming. Existing 29 tests stay green.
- **On-device, without the app:** `scripts/ble_send.py` (Python + `bleak`)
  connects to `"FlightRadar"`, packs a test packet (a known center + a couple of
  aircraft), and writes it. Verify: the device shows `B`, switches to the
  BLE-supplied aircraft, and centers on the packet coordinates; after
  `BLE_FRESHNESS_MS` with no further writes it falls back to the stale marker;
  bringing Wi-Fi back flips the source to `W`.

## Out of scope (→ sub-project B)

The mobile companion app: platform choice (native vs Web Bluetooth), GPS
acquisition, airplanes.live fetching on the phone, and the UI. B implements the
sender side of the protocol defined above. `scripts/ble_send.py` is the interim
sender used to validate A.
