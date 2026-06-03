# Architecture

Flight Radar is split into a **pure, host-testable core** and a thin **Arduino
firmware layer**. Everything that can be reasoned about without hardware —
parsing, geometry, formatting — lives in headers with no Arduino dependencies,
so it compiles and is unit-tested on your laptop under PlatformIO's `native`
environment. The `.ino` only does I/O: Wi-Fi, HTTP, the display, touch, and the
state machine that ties them together.

```
   airplanes.live (HTTPS JSON)        phone companion (BLE write, fallback)
                     │                          │
   flight_core.h  ───┤              ble_core.h ─┤   parse binary packet
   parse+dist+sort   │              (pure, tested)  + dist + sort
                     ▼                          ▼
                  vector<Aircraft> (nearest first, capped)
                     │   ← source arbitration: Wi-Fi primary, BLE fallback
                     ▼
   render_core.h  ──────────────  bearing + projection + text (pure, tested)
                     │  screen coords + display strings
                     ▼
   flight_ticker.ino ───────────  TFT_eSPI sprite + state machine (firmware)
        ▲                                   │
        └──── cst816s.h (touch, INT ISR) ───┘
```

## Modules

### `src/flight_core.h` — data (pure, Arduino-free)
- `struct Aircraft` — callsign, type, altitude (ft), on-ground flag, ground
  speed (kt), `track` (true heading, degrees), `squawk` (transponder code),
  lat/lon, and `distKm` (filled during parse).
- `parseNearest(json, myLat, myLon, maxN, hideGround)` — parses the
  airplanes.live response with an ArduinoJson filter (only the fields we use,
  now including `track` and `squawk`), computes great-circle distance to each
  aircraft via `haversineKm`, sorts nearest-first, and caps to `maxN`. With
  `hideGround` set it drops on-ground aircraft before the sort/cap. Returns an
  empty vector on malformed JSON.
- Unit helpers `ftToM`, `ktToKmh`.

### `src/render_core.h` — presentation math (pure, Arduino-free, host-tested)
- `bearingDeg(lat1,lon1,lat2,lon2)` — initial great-circle bearing, north = 0°,
  clockwise, normalized to `[0,360)`.
- `polarToXY(bearing, distKm, rangeKm, cx, cy, maxRadiusPx)` → `ScreenPoint` —
  maps a (bearing, distance) to a pixel. North is up; distance is clamped to the
  ring range and scaled linearly to the radius.
- `compassPoint(bearing)` — 8-point rose label (`N`, `NE`, … `NW`).
- `fmtDist` / `fmtAlt` / `fmtSpeed` — short display strings (with clamps,
  ground/NaN handling), reused units from `flight_core.h`.
- `vectorEnd(from, headingDeg, length)` — endpoint of a fixed-length line from a
  blip along its heading (north-up), used to draw the per-aircraft track vector.
- `altBand(altFt, onGround)` — altitude band index `0..5` (ground/unknown, <3k,
  3–10k, 10–25k, 25–40k, >40k ft) that selects the blip color.
- `isEmergencySquawk(code)` — true for 7500/7600/7700.

Keeping these pure is what makes the project testable: see
`test/test_core/test_main.cpp` (43 cases — cardinal bearings, projection
clamping and off-axis geometry, compass rounding boundaries, formatter edge
cases, vector/altitude-band/emergency-squawk helpers, the parse/sort/distance
tests, plus the BLE packet parser).

### `src/ble_core.h` — BLE wire protocol + parser (pure, Arduino-free, host-tested)
- `parseBlePacket(buf, len, maxN, hideGround)` → `BlePacket` — decodes one
  binary packet (v2 format below), fills each aircraft's `distKm` via
  `haversineKm` from the packet's center, sorts nearest-first, and caps to
  `maxN`. With `hideGround` set it drops on-ground aircraft. Returns
  `ok = false` on any validation failure (bad magic/version, count over the
  cap, or a length that doesn't match `header + count·record`).
- Like `flight_core.h`, this is pure and runs under `native` (valid decode, bad
  magic, bad version, count overflow, length mismatch, flag handling including
  `track`/`squawk`, hide-ground, and cap-to-maxN).

### `src/cst816s.h` — touch driver (Arduino)
Minimal I2C driver for the CST816S capacitive controller (address `0x15`). Reads
the gesture register (`0x01`) and exposes a `TouchGesture` enum
(`TG_CLICK`, `TG_LEFT`, `TG_RIGHT`, `TG_DOWN`, …). Returns `TG_NONE` on any I2C
error, so a missing/asleep chip never blocks rendering.

### `src/flight_ticker.ino` — firmware
- **Networking** — `connectWifi()` and `pollApi()` (HTTPS via `WiFiClientSecure`
  + `setInsecure()`, because airplanes.live 301-redirects HTTP). Polled on a
  non-blocking 15 s `millis` timer in both views.
- **Rendering** — a full-screen 240×240 `TFT_eSprite` is the framebuffer; every
  frame is drawn into the sprite and `pushSprite`'d in one shot (no flicker).
  `drawRadar()` and `drawDetail()` are the two renderers. `drawRadar()` colors
  each blip by `altBand()` (via the `kAltColors` table), draws a `vectorEnd()`
  heading line, rings + labels the nearest aircraft in white, and — for any
  `isEmergencySquawk()` aircraft — blinks it red and shows an `EMERGENCY <code>`
  banner.
- **Touch** — a falling-edge ISR latches INT events; `handleTouch()` reads the
  gesture once per event with a 300 ms debounce (see Hardware notes for why).
- **BLE ingest** — a NimBLE peripheral (`IngestCallbacks::onWrite`) receives
  binary packets from a phone; the callback only buffers the bytes and sets a
  flag, while `loop()` does the parse. See "BLE data path" below.
- **State machine** — see below.

## Data flow per poll

1. `pollApi()` requests `/v2/point/{lat}/{lon}/{RADIUS_NM}`.
2. `parseNearest()` turns the JSON into a sorted, capped `vector<Aircraft>`
   stored in `g_cache`; `g_idx` (the detail-carousel cursor) is clamped to the
   new size.
3. On failure, `g_stale` is set — the radar keeps drawing the last snapshot with
   a red stale dot.

Rendering reads `g_cache` every frame independently of the poll, so the sweep
animates smoothly between polls.

## BLE data path (fallback)

Wi-Fi is primary; BLE is a fallback for when the device has no Wi-Fi (e.g. away
from a known network, fed by a phone over Bluetooth instead). A phone companion
writes one compact binary packet to a GATT characteristic; the radar re-centers
on the packet's GPS and plots its aircraft. The phone side is the Flutter
companion app (`companion/`, Android + iOS) — see "Phone companion" below.

**GATT** (NimBLE peripheral, always advertising, open — no pairing):
- Device name: `FlightRadar`
- Service UUID: `f1a90001-7e1d-4c2a-9b3f-1a2b3c4d5e6f`
- One write characteristic (`WRITE | WRITE_NR`):
  `f1a90002-7e1d-4c2a-9b3f-1a2b3c4d5e6f`

**Wire format** (v2, little-endian; both ESP32 and host are LE). A 12-byte
header followed by `count` × 32-byte records:

| Header (12 B) | Field | Bytes |
|---|---|---|
| `0` | magic `0x46 0x52` (`'F' 'R'`) | 2 |
| `2` | version (`2`) | 1 |
| `3` | count (uint8, ≤ 15) | 1 |
| `4` | center lat (float32) | 4 |
| `8` | center lon (float32) | 4 |

| Record (32 B) | Field | Bytes |
|---|---|---|
| `0` | callsign, ASCII, space-padded | 8 |
| `8` | type, ASCII, space-padded | 4 |
| `12` | lat (float32) | 4 |
| `16` | lon (float32) | 4 |
| `20` | altitude ft (int32) | 4 |
| `24` | ground speed kt (int16) | 2 |
| `26` | flags (uint8) | 1 |
| `27` | pad | 1 |
| `28` | track deg (int16) | 2 |
| `30` | squawk (uint16) | 2 |

Flag bits: `GROUND = 0x01`, `ALT_VALID = 0x02`, `GS_VALID = 0x04`,
`TRACK_VALID = 0x08`, `SQUAWK_VALID = 0x10`. When a `*_VALID` bit is clear, the
parser stores `NaN` (or `0` for squawk) for that field and the UI shows it as
N/A. Up to **15** aircraft fit on the wire: a full 16-record v2 packet (524 B)
would exceed the single-write ATT payload (~514 B at MTU 517), whereas 15
records = 492 B fits in one write. The display still caps to `MAX_AIRCRAFT`
(10). `parseBlePacket` validates magic/version/count/length, fills distance,
sorts nearest-first, and caps — see `src/ble_core.h`.

**Source arbitration** (in `loop()`, every frame): Wi-Fi wins whenever it's
connected; otherwise BLE is used if a packet arrived within `BLE_FRESHNESS_MS`
(default 30 s); otherwise the source is "none". A `g_bleLastRx != 0` guard keeps
BLE dormant until a real packet arrives (so an uninitialized timestamp at boot
isn't read as "fresh"). The bottom-center indicator reflects the live source:
green `W` (Wi-Fi), red `W` (Wi-Fi up, poll failing), cyan `B` (BLE), red
`NO LINK`.

**Concurrency.** The BLE write callback runs in the NimBLE task. It does the
minimum: copy the bytes into a buffer and set `g_blePacketReady`. `loop()` (the
same single thread that renders) does the actual `parseBlePacket` and updates
`g_cache` / the radar center. So the render path never races the BLE task — no
locking needed, the flag is the handoff.

**Phone companion** (`companion/`). A Flutter app for **Android and iOS**
(hardware-verified) that polls airplanes.live around the phone's own GPS,
encodes the nearby aircraft as a v2 packet, and writes it to the device's GATT
characteristic — the production sender for this fallback path (the device cannot
reach Wi-Fi but the phone can). It's built around a shared `GatewayEngine`
(poll -> encode -> BLE-write loop, in `lib/service/`) plus per-platform drivers
so it keeps running in the background: an Android foreground service
(`flutter_foreground_task`) and an iOS continuous-background-location keep-alive
(`ios_gateway_driver.dart`). Stack: `flutter_blue_plus` (BLE), `geolocator`
(GPS), `permission_handler`; the Dart packet encoder in `lib/packet/` mirrors
`src/ble_core.h`. `scripts/ble_send.py` remains the laptop smoke-test harness.

## State machine

```
        tap
 RADAR ───────────────────────────────►  DETAIL
   ▲                                        │
   │  tap / swipe-down / 15 s idle          │ swipe-left  → next aircraft
   └────────────────────────────────────── │ swipe-right → previous (wraps)
                                            │ (g_idx cycles through g_cache)
```

- **RADAR**: rings + sweep + blips; nearest highlighted with callsign; observer
  dot at center; "NO TRAFFIC" when the list is empty; red dot when stale.
- **DETAIL**: one aircraft (callsign, type + compass arrow, distance,
  altitude/speed) with page dots. Auto-returns to RADAR after 15 s of no touch,
  and bounces back immediately if the list becomes empty.

## Configuration

Tunables live in `src/config.h` (copied from `config.example.h`, gitignored):
Wi-Fi credentials, observer `MY_LAT`/`MY_LON`, `RADIUS_NM` (search radius, also
drives the radar range in km), `POLL_INTERVAL_MS`, `MAX_AIRCRAFT` (blips +
carousel length), `IDLE_RETURN_MS`, `SWEEP_PERIOD_MS`, `BLE_FRESHNESS_MS` (how
long BLE-fed data counts as live before falling back to `NO LINK`),
`HIDE_GROUND_AIRCRAFT` (default `1`; drops on-ground aircraft from the radar and
list on both the Wi-Fi and BLE paths), and the touch pins.

## Extending

- **More/fewer aircraft** → `MAX_AIRCRAFT`. Radar range → `RADIUS_NM`.
- **New display field** → add a pure formatter to `render_core.h` (with a test),
  then draw it in `drawDetail()`.
- **New gesture action** → add a case in `handleTouch()`; the gesture codes are
  in `cst816s.h`.
- Anything you add to the pure headers should come with a `native` test — that's
  the layer that catches logic regressions without a board.

## Why the layering

You can run the entire logic surface (`pio test -e native -f test_core`) in
~0.3 s on a laptop, with no hardware, no Wi-Fi, no display. Bugs in bearing math
or formatting are caught there; the firmware layer is left small enough to verify
by flashing and watching the screen.
