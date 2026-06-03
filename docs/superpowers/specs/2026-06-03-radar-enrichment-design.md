# Radar Enrichment — Heading Vectors + Altitude Color + Emergency Squawk — Design

> **Status:** approved design. Adds three radar-display features that need two new
> data fields (track, squawk), carried on BOTH the Wi-Fi and BLE paths (BLE wire
> format bumped to v2 for parity).

## Purpose

Make the radar more informative at a glance:
1. **Heading vectors** — a short line from each blip pointing where the aircraft is going.
2. **Altitude color** — blips colored by altitude band.
3. **Emergency squawk** — aircraft squawking 7500/7600/7700 blink red with a banner.

Decided in brainstorm: **full parity on both data paths** (Wi-Fi poll AND BLE
packet from the phone companion), so the BLE wire format gains the two new fields
(v2).

## New data fields

Two fields added to the `Aircraft` model (firmware `flight_core.h` and Dart
`aircraft.dart`):
- **`track`** — true track in degrees (0–359). Unknown → NAN (firmware) / null (Dart).
  Source: airplanes.live `track`.
- **`squawk`** — the 4-digit transponder code as an integer (e.g. 7700). Unknown →
  0 / null. Source: airplanes.live `squawk` (a string; parse to int).

`altFt` already exists. (Field names on airplanes.live to confirm during
implementation: `track`, `squawk` — both standard /v2/point fields.)

## Data extraction

- Firmware `parseNearest` (Wi-Fi): extract `track` (numeric) and `squawk`
  (string → int) into each `Aircraft`. Add `track`/`squawk` to the JSON filter.
- App `parseAircraft`: same extraction into the Dart `Aircraft`.

## BLE wire format v2 (parity)

The current v1 record is 28 bytes; v2 grows it to **32 bytes** by appending the
two fields, and bumps the version. Layout:

**Header (12 B, unchanged):** magic `FR`, version (now **2**), count, center lat/lon f32.

**Record (32 B):**

| Offset | Field | Type | Notes |
|---|---|---|---|
| 0 | callsign | 8 B ascii | unchanged |
| 8 | type | 4 B ascii | unchanged |
| 12 | lat | float32 | unchanged |
| 16 | lon | float32 | unchanged |
| 20 | altFt | int32 | unchanged |
| 24 | gsKt | int16 | unchanged |
| 26 | flags | uint8 | unchanged bits + 2 new |
| 27 | pad | uint8 | unchanged (0) |
| **28** | **track** | **int16** | degrees 0–359; valid iff TRACK_VALID |
| **30** | **squawk** | **uint16** | decimal code (e.g. 7700); valid iff SQUAWK_VALID |

**Flags:** existing `GROUND=0x01`, `ALT_VALID=0x02`, `GS_VALID=0x04`; new
`TRACK_VALID=0x08`, `SQUAWK_VALID=0x10`.

**Constants:** `BLE_VERSION = 2`, `BLE_RECORD_SIZE = 32`. ⚠️ **`BLE_MAX_AIRCRAFT`
reduced 16 → 15**: a full v2 packet at 16 records is `12 + 16·32 = 524` B, over the
single-write ATT payload (514 B at MTU 517); 15 records = `12 + 15·32 = 492` B fits
comfortably. The display cap (`MAX_AIRCRAFT = 10`) is unchanged, so 15 is ample
headroom. `BLE_MAX_PACKET = 12 + 15·32 = 492`.

Both sides are under our control (mono-repo), so the firmware parser may reject
non-v2 packets. The app encoder writes v2; `parseAircraft` supplies track/squawk
(with the valid flags) to the packet.

## Render features (firmware)

All three layer onto the existing `drawRadar`/`drawDetail`. New pure helpers go in
`render_core.h` (host-tested); the `.ino` maps them to TFT colors/draw calls.

### Heading vectors
A short fixed-length line (~10 px) from each blip toward its `track`. North-up, so
screen angle = track (0° = up/north, 90° = right/east). New pure helper:
`ScreenPoint vectorEnd(ScreenPoint from, double headingDeg, double length)` →
`from.x + length·sin(rad)`, `from.y − length·cos(rad)`. No track (invalid) → no
line. `drawRadar` draws the line per blip.

### Altitude color
Blips are colored by altitude band. A pure `int altBand(double altFt, bool onGround)`
returns a band index; the `.ino` maps band → TFT color. Proposed bands (ft):
- ground / unknown → grey
- < 3000 → red
- 3000–10000 → orange
- 10000–25000 → yellow-green
- 25000–40000 → cyan
- > 40000 → blue

The **nearest** aircraft keeps its distinction: larger dot + callsign label + a
**white outline ring** (instead of the current solid yellow). Other blips: filled
with their altitude-band color.

### Emergency squawk
A pure `bool isEmergencySquawk(int code)` → `code == 7500 || 7600 || 7700`. For an
emergency aircraft: the blip **blinks red** (toggle on a ~500 ms phase off the
existing sweep clock, overriding the altitude color) and a bottom banner shows
`EMERGENCY <code>` (e.g. `EMERGENCY 7700`). Works on both paths via v2.

## Detail carousel

`drawDetail` optionally shows `track` (e.g. as a compass/°) and the squawk; minor,
reusing existing layout. (Route origin→destination is a SEPARATE later feature, not
this spec.)

## Testing (TDD, host)

Pure unit tests:
- `parseNearest` extracts `track` + `squawk` (Unity, flight_core).
- `parseBlePacket` v2 round-trip incl. track/squawk + TRACK/SQUAWK valid flags, and
  the new 15-cap (Unity, ble_core).
- App `parseAircraft` extracts track/squawk (Dart).
- App `encodePacket` v2 byte layout incl. track@28 / squawk@30 / new flags (Dart) —
  keep byte-parity with the firmware parser.
- `vectorEnd`, `altBand`, `isEmergencySquawk` (Unity, render_core) — pure helpers.

On-device (eyes): vectors point sensibly, colors read by altitude, an injected
emergency squawk blinks + banners (test via `ble_send.py` with a 7700 record).

## Out of scope

- Route origin→destination in the detail view (next feature; may use the paid
  Flightradar24 API).
- Speed-scaled vectors, configurable color bands, heading-up orientation.

## Done criteria

- `pio test -e native -f test_core` and `flutter test` green with the new cases
  (incl. v2 round-trip and the pure helpers).
- `pio run -e esp32-s3` compiles; `ble_send.py` updated to v2.
- On device: blips show heading vectors, are colored by altitude (nearest = white
  ring + label), and a 7700/7600/7500 aircraft blinks red with an `EMERGENCY` banner
  — on both the Wi-Fi and BLE-fed paths.
