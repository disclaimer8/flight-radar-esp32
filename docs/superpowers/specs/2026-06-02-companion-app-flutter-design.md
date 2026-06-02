# Flight Radar Companion App (sub-project B) — Design

> **Status:** approved design, v1 = Android. iOS is a deferred phase 2 (same codebase).
> Pairs with the shipped firmware BLE fallback path (sub-project A).

## Purpose

The Flight Radar device (Waveshare ESP32-S3) shows a live aircraft radar. Its
primary data path is Wi-Fi → airplanes.live. When there is no Wi-Fi, a phone
**companion app** acts as the gateway: it reads the phone's GPS, fetches nearby
aircraft from airplanes.live over cellular, packs them into the device's compact
binary BLE packet, and writes that packet to the device's GATT ingest
characteristic. The radar then re-centers on the phone's location and plots the
aircraft. The device already implements the BLE peripheral side; this project is
the phone side.

The app must keep feeding the device **in the background** (screen off / app not
foregrounded), because the device sits on a desk/dashboard while the phone stays
in a pocket.

## Scope

- **v1 (this spec): Android only.** Full pipeline — GPS → fetch → encode → BLE
  write — running in a background foreground-service.
- **Phase 2 (separate spec): iOS.** Same Dart codebase; only the keep-alive
  strategy and provisioning differ (see §10). Architecture is designed so iOS
  drops in without restructuring.
- **Out of scope:** any on-phone radar UI / map; pairing security; multi-device;
  historical logging.

## Tech stack

- **Flutter (Dart).** Single codebase, strongest background BLE + location plugin
  ecosystem.
- `flutter_blue_plus` — BLE central (scan / connect / write).
- `geolocator` — GPS position stream (also the background keep-alive driver).
- `flutter_foreground_task` — Android foreground service + persistent
  notification (hosts the gateway loop).
- `http` — airplanes.live fetch.
- `flutter_test` / `test` — unit tests for the pure codec.

All chosen libraries are free and mainstream.

## Project location

A new **`companion/`** subdirectory in the existing `flight-radar-esp32`
repository (mono-repo). The BLE wire protocol is shared between the firmware
(`src/ble_core.h`) and the app's Dart codec; co-locating them keeps the two in
sync. A separate repo would risk format drift.

## Architecture — components

Each unit has one responsibility, a defined interface, and is testable in
isolation. The pure codec is the tested core, mirroring how the firmware isolates
`flight_core.h` / `render_core.h` from the Arduino layer.

| Unit | File | Responsibility | Depends on |
|---|---|---|---|
| **Packet codec** | `lib/packet/ble_packet.dart` | Pure: `encodePacket(centerLat, centerLon, List<Aircraft>) → Uint8List`. Wire constants. No Flutter imports. | `aircraft.dart` |
| **Aircraft model** | `lib/data/aircraft.dart` | Plain data: callsign, type, lat, lon, altFt?, gsKt?, onGround. | — |
| **airplanes.live client** | `lib/data/airplanes_client.dart` | `fetchNearby(lat, lon, radiusNm) → List<Aircraft>`; HTTP GET + JSON→Aircraft mapping. | `aircraft.dart`, `http` |
| **BLE manager** | `lib/ble/ble_manager.dart` | Scan by service UUID, connect to `FlightRadar`, write packet (with response), auto-reconnect, expose connection state. | `flutter_blue_plus` |
| **Location service** | `lib/location/location_service.dart` | GPS position stream; background-capable. Behind an interface so iOS can swap implementation. | `geolocator` |
| **Gateway service** | `lib/service/gateway_service.dart` | The orchestration loop, hosted in the foreground service. Ties location → client → codec → BLE. | all above, `flutter_foreground_task` |
| **Home screen (UI)** | `lib/ui/home_screen.dart` | Minimal status UI + Start/Stop. | gateway state |
| **App entry** | `lib/main.dart` | Wire-up, permission requests. | — |

## Wire format (must match `src/ble_core.h` byte-for-byte)

Little-endian. 12-byte header + `count` × 28-byte records.

**Header (12 B):**

| Offset | Field | Type |
|---|---|---|
| 0 | magic `0x46 0x52` (`'F' 'R'`) | 2 × uint8 |
| 2 | version = `1` | uint8 |
| 3 | count (≤ 16) | uint8 |
| 4 | center latitude | float32 |
| 8 | center longitude | float32 |

**Record (28 B):**

| Offset | Field | Type |
|---|---|---|
| 0 | callsign, ASCII, space-padded | 8 bytes |
| 8 | type, ASCII, space-padded | 4 bytes |
| 12 | latitude | float32 |
| 16 | longitude | float32 |
| 20 | altitude ft | int32 |
| 24 | ground speed kt | int16 |
| 26 | flags | uint8 |
| 27 | pad (0) | uint8 |

**Flags:** `GROUND = 0x01`, `ALT_VALID = 0x02`, `GS_VALID = 0x04`. When a
`*_VALID` bit is clear, the firmware stores NaN for that field. The encoder sets
the valid bit only when the source value is present.

**Caps:** at most 16 records on the wire (`BLE_MAX_AIRCRAFT`); send the nearest
16. The device further caps the display to `MAX_AIRCRAFT` (10). The encoder
itself does not sort/cap by distance — it encodes what it is given — but the
gateway passes a list already trimmed to ≤ 16 nearest (the firmware sorts
nearest-first on receipt regardless).

## GATT (device side, fixed)

- Device name: `FlightRadar`
- Service UUID: `f1a90001-7e1d-4c2a-9b3f-1a2b3c4d5e6f`
- Write characteristic (`WRITE | WRITE_NR`): `f1a90002-7e1d-4c2a-9b3f-1a2b3c4d5e6f`
- Open (no pairing). Always advertising.
- Write **with response** (verified to reliably carry the full packet regardless
  of negotiated MTU).

## Data flow (background loop)

```
foreground service (persistent notification)
  every ~10 s, and on significant location change:
    1. get current GPS fix
    2. fetch airplanes.live /v2/point/{lat}/{lon}/{radiusNm}
    3. map JSON → List<Aircraft>, take nearest ≤ 16
    4. encodePacket(GPS, aircraft) → Uint8List
    5. write to the connected FlightRadar characteristic
```

Cadence ~10 s is well inside the device's 30 s freshness window, so the device
shows a steady cyan **B** while the app runs.

## airplanes.live client

- `GET https://api.airplanes.live/v2/point/{lat}/{lon}/{radiusNm}` (same endpoint
  the firmware uses). Public, no auth, ≤ 1 req/s.
- Map each JSON aircraft, mirroring the firmware's `flight_core.h` extraction:
  callsign (`flight`, trimmed) → callsign; `t` → type; `lat`/`lon`; `alt_baro`
  (numeric) → altFt with ALT_VALID, `"ground"` → onGround + alt invalid; `gs` →
  gsKt with GS_VALID. Missing fields → corresponding valid bit clear.
- **Sort + trim:** `fetchNearby` returns the aircraft **sorted nearest-first** by
  haversine distance from the query center, **trimmed to ≤ 16**. This requires a
  small pure `haversineKm(lat1, lon1, lat2, lon2)` helper (mirror of
  `flight_core.h`), placed in `lib/data/aircraft.dart` or a `lib/data/geo.dart`
  util and unit-tested. The wire record carries no distance field (the device
  recomputes it), so distance is used only for selection/order here.
- `radiusNm` configurable; default ~50 nm (device shows nearest 10 anyway).
- On HTTP/parse error or no connectivity: return empty / skip the cycle; never
  crash the loop; keep the BLE connection.

## Background strategy (Android v1)

- `flutter_foreground_task` runs a foreground service with a persistent
  notification ("Feeding Flight Radar…"). The gateway loop lives in the service
  isolate.
- `geolocator` with background location keeps the process alive and supplies the
  GPS center.
- BLE connection is held open by the service across the cycles.
- **Permissions:** Location (fine + background / "Allow all the time"), Bluetooth
  scan + connect (Android 12+ runtime permissions), notifications (Android 13+).
- **Battery:** continuous location + BLE is a real battery cost — acknowledged
  and surfaced to the user (it is the price of background operation).

## UI (minimal — YAGNI)

A single home screen:
- Connection status: `disconnected` / `scanning` / `connected to FlightRadar`.
- Start / Stop toggle (starts/stops the foreground service).
- Last-sent timestamp + count of aircraft in the last packet.
- Current GPS coordinates (or "no fix").
- Permission prompts / a clear screen when a permission is denied.

No on-phone radar or map in v1.

## Error handling

| Condition | Behavior |
|---|---|
| BLE disconnect | Auto-reconnect (re-scan + connect); status reflects it. |
| No GPS fix | Skip the cycle (don't send a stale/zero center). |
| Fetch failure / offline | Skip the cycle; keep BLE connection; retry next tick. |
| Permission denied | Stop the loop; show an explanatory prompt to re-grant. |
| Device not found on scan | Stay in `scanning`; keep retrying. |

## Testing

- **Unit (pure codec):** `encodePacket` round-trip against a known-good byte
  buffer that matches `src/ble_core.h` and `scripts/ble_send.py` (same 3-aircraft
  sample, byte-for-byte). Cover: header bytes, record offsets, flag/NaN handling,
  empty list, cap at 16.
- **Client mapping:** map a captured airplanes.live JSON sample → Aircraft and
  assert field extraction (incl. ground / missing-field cases), plus
  nearest-first sort + trim-to-16.
- **Geo helper:** `haversineKm` against known city-pair distances.
- **Integration:** run against the real flashed device; confirm it shows cyan B,
  re-centers, and plots the sent aircraft (Wi-Fi off). The device is already
  hardware-verified, so it is a reliable test target.
- A **fake-aircraft mode** (toggle) emits a synthetic packet without internet,
  for testing the BLE + background path in isolation.

## iOS (phase 2 — not this spec)

Same Dart codebase. Differences only in:
- Keep-alive: iOS background modes `location` + `bluetooth-central`, "Always"
  location permission, continuous-location keep-alive instead of a foreground
  service.
- Provisioning: a paid Apple Developer account ($99/yr) for non-expiring
  installs and the required background entitlements.

The `LocationService` and `GatewayService` are kept behind interfaces in v1 so
the iOS keep-alive implementation drops in without touching the codec, client,
BLE manager, or UI.

## Done criteria (v1)

- `companion/` Flutter project builds and runs on Android.
- Codec unit tests pass and match the firmware wire format byte-for-byte.
- With the device flashed and Wi-Fi off, starting the app makes the device show
  cyan **B**, re-center on the phone's GPS, and plot nearby real aircraft, and it
  keeps doing so with the app backgrounded / screen off.
