# Hide Ground Aircraft (config toggle) — Design

> **Status:** approved design. Small cross-subsystem change (firmware + companion app).

## Purpose

Aircraft sitting on the ground clutter the radar and the detail carousel. Hide
them — on both the radar blips and the tap-to-detail list — on both data paths
(Wi-Fi poll and BLE packet). Make it a config toggle (default: hide).

## Signal

An aircraft is "on the ground" when its `onGround` flag is set:
- Wi-Fi (airplanes.live): `alt_baro == "ground"` → `onGround = true`.
- BLE packet: the `GROUND` flag bit (0x01) → `onGround = true`.

Both already populate `Aircraft.onGround` (firmware `flight_core.h` / `ble_core.h`,
Dart `aircraft.dart`).

## Where to filter

At the **parse/selection stage**, before the nearest-N sort+cap, so that (a)
ground aircraft never consume one of the N nearest slots (their slots go to
airborne traffic), and (b) they never reach `g_cache` — so they're absent from
both `drawRadar` (blips) and `drawDetail` (carousel) with **no render-side
change**. Same logic on the app side keeps the BLE packet airborne-only.

## Key constraint: keep the pure functions config-agnostic

`parseNearest`, `parseBlePacket` (firmware) and `parseAircraft` (app) are pure,
host-tested functions. They must NOT read `config.h` / a global, or the native
unit tests (which don't include config) break and the functions stop being
testable with both settings. So the toggle is passed **as a parameter**; the
caller supplies it from config.

## Design

### Firmware
- `src/config.h` + `src/config.example.h`: add `#define HIDE_GROUND_AIRCRAFT 1`
  (default on).
- `src/flight_core.h` — `parseNearest(json, lat, lon, maxN, bool hideGround)`:
  when `hideGround`, skip aircraft with `onGround == true` before the distance
  sort + `maxN` cap.
- `src/ble_core.h` — `parseBlePacket(buf, len, maxN, bool hideGround)`: same skip
  before sort + cap (defensive: the app already filters, but the device stays the
  display authority).
- `src/flight_ticker.ino`: pass `HIDE_GROUND_AIRCRAFT` to both call sites.

### Companion app
- `companion/lib/data/airplanes_client.dart`: a top-level `const bool
  kHideGroundAircraft = true;`. `parseAircraft(body, centerLat, centerLon,
  {bool hideGround = true})` skips `onGround` aircraft before sort + cap.
  `AirplanesClient.fetchNearby` passes `kHideGroundAircraft`.

Both default to hide. They're independent toggles (firmware config vs app const);
setting both consistently is expected. The firmware config is the display
authority — even if the app sent ground aircraft, the firmware's `parseBlePacket`
filter (when on) drops them.

## Testing (TDD)

- `test/test_core/test_main.cpp` (Unity): for `parseNearest` and `parseBlePacket`,
  add cases proving `hideGround=true` excludes ground aircraft AND fills the freed
  nearest-N slots with airborne ones (a ground aircraft that is nearer than an
  airborne one must not displace it); and `hideGround=false` keeps ground (current
  behavior).
- `companion/test/airplanes_client_test.dart`: `parseAircraft` with
  `hideGround=true` excludes the ground entry and keeps airborne ordering;
  `hideGround=false` keeps it.

## Out of scope

- A runtime/on-screen toggle (compile-time config only).
- Altitude-threshold filtering (only the explicit `onGround` flag).

## Done criteria

- `pio test -e native -f test_core` and `flutter test` green with the new cases.
- On device (default config): aircraft on the ground appear neither as radar blips
  nor in the detail carousel, on both Wi-Fi and BLE paths. Flipping
  `HIDE_GROUND_AIRCRAFT` to 0 restores them on the Wi-Fi path.
