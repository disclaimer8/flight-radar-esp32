# Architecture

Flight Radar is split into a **pure, host-testable core** and a thin **Arduino
firmware layer**. Everything that can be reasoned about without hardware —
parsing, geometry, formatting — lives in headers with no Arduino dependencies,
so it compiles and is unit-tested on your laptop under PlatformIO's `native`
environment. The `.ino` only does I/O: Wi-Fi, HTTP, the display, touch, and the
state machine that ties them together.

```
            airplanes.live (HTTPS JSON)
                     │
                     ▼
   flight_core.h  ──────────────  parse + distance + sort   (pure, tested)
                     │  vector<Aircraft> (nearest first, capped)
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
  speed (kt), lat/lon, and `distKm` (filled during parse).
- `parseNearest(json, myLat, myLon, maxN)` — parses the airplanes.live response
  with an ArduinoJson filter (only the fields we use), computes great-circle
  distance to each aircraft via `haversineKm`, sorts nearest-first, and caps to
  `maxN`. Returns an empty vector on malformed JSON.
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

Keeping these pure is what makes the project testable: see
`test/test_core/test_main.cpp` (29 cases — cardinal bearings, projection
clamping and off-axis geometry, compass rounding boundaries, formatter edge
cases, plus the original parse/sort/distance tests).

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
  `drawRadar()` and `drawDetail()` are the two renderers.
- **Touch** — a falling-edge ISR latches INT events; `handleTouch()` reads the
  gesture once per event with a 300 ms debounce (see Hardware notes for why).
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
carousel length), `IDLE_RETURN_MS`, `SWEEP_PERIOD_MS`, and the touch pins.

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
