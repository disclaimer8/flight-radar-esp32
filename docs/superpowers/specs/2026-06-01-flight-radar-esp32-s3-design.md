# Flight Radar — ESP32-S3-Touch-LCD-1.28 (Design)

**Date:** 2026-06-01
**Status:** Approved (brainstorming), ready for implementation plan
**Supersedes display layer of:** `2026-05-30-flight-ticker-design.md` (16×2 LCD ticker)

## Summary

Repurpose the Flight Ticker firmware onto a new round touch display
(Waveshare ESP32-S3-Touch-LCD-1.28). Instead of scrolling text on a 16×2
character LCD, render a **North-up radar**: the observer at center, nearby
aircraft as blips placed by bearing and distance, with a rotating sweep.
Tapping the radar opens a **detail carousel** — one flight at a time, swipe
to page through the nearest aircraft.

The polling/parsing/distance core (`flight_core.h`) is reused unchanged; all
new work is a pure render-math module plus a graphics/touch firmware layer.

## Hardware (confirmed)

Board: **Waveshare ESP32-S3-Touch-LCD-1.28**
- MCU: ESP32-S3R2 (dual LX7, up to 240 MHz), **2 MB PSRAM**, 16 MB flash, 512 KB SRAM.
- Display: **GC9A01A**, 240×240 round, SPI.
- Touch: **CST816S**, I2C.
- IMU: QMI8658C (present, **unused** in this design).
- Battery: MX1.25, ETA6096 charger, VBAT sense on GPIO1 (unused here).

### Pin map

| Function        | GPIO |
|-----------------|------|
| LCD MOSI        | 11   |
| LCD SCLK        | 10   |
| LCD CS          | 9    |
| LCD DC          | 8    |
| LCD RST         | 14   |
| LCD Backlight   | 2    |
| Touch I2C SDA   | 6    |
| Touch I2C SCL   | 7    |
| Touch INT       | 5    |
| Touch RST       | 13   |

PSRAM (2 MB) comfortably holds a 240×240×16-bit framebuffer (~115 KB).

## Constraints

- **North-up only.** No magnetometer on the board (IMU is accel+gyro), so the
  radar cannot rotate to physical heading. Top of screen = geographic North;
  the per-aircraft arrow is the bearing from the observer's coordinates.
- API radius is in nautical miles (existing `RADIUS_NM`); the radar range and
  ring labels are derived in km (`RADIUS_NM × 1.852`).
- API rate limit 1 req/s; firmware polls every 15 s (unchanged).
- airplanes.live forces HTTPS (Cloudflare 301) → keep `WiFiClientSecure` +
  `setInsecure()`.

## Architecture

```
flight_core.h        (REUSED, unchanged, Arduino-free, host-tested)
  parseNearest() → sorted vector<Aircraft> with distKm
        │
        ▼
render_core.h        (NEW, Arduino-free, host-tested: pure math + formatting)
  bearingDeg(), polarToXY(), compassPoint(), detail field formatters
        │
        ▼
flight_ticker.ino    (Arduino: WiFi/HTTP reuse + TFT_eSPI sprite + CST816S + state machine)
```

### Component 1 — `render_core.h` (new, pure, host-testable)

No Arduino includes; compiles under `[env:native]` and is unit-tested with Unity.

- `double bearingDeg(double lat1, double lon1, double lat2, double lon2)`
  Initial great-circle bearing from observer→aircraft, normalized to `[0,360)`,
  north = 0, increasing clockwise.
- `struct ScreenPoint { int x; int y; }`
  `ScreenPoint polarToXY(double bearingDeg, double distKm, double rangeKm, int cx, int cy, int maxRadiusPx)`
  Distance clamped to `[0, rangeKm]`; radius scales linearly. Screen Y grows
  downward, north points up: `x = cx + r·sin(θ)`, `y = cy − r·cos(θ)`.
- `const char* compassPoint(double bearingDeg)`
  8-point rose, Cyrillic labels for UI: С, СВ, В, ЮВ, Ю, ЮЗ, З, СЗ.
- Detail field formatters returning short strings (no fixed-width padding):
  - distance → e.g. `"6 km"` (rounded, clamped 0..999)
  - altitude → `"3650м"` / `"GND"` / `"---"` (ft→m, reuse `ftToM`)
  - speed → `"820"` / `"---"` (kt→km/h, reuse `ktToKmh`)

### Component 2 — `flight_ticker.ino` (graphics + interaction)

- **Networking:** reuse existing WiFi connect + `WiFiClientSecure`/`setInsecure()`
  + `HTTPClient` polling of `https://api.airplanes.live/v2/point/{lat}/{lon}/{RADIUS_NM}`,
  parsed via `parseNearest()`. Non-blocking 15 s `millis` timer.
- **Graphics:** TFT_eSPI driving GC9A01; a single full-screen `TFT_eSprite`
  (240×240, 16-bit) allocated in PSRAM as the framebuffer. Every frame is drawn
  into the sprite, then `pushSprite(0,0)` — no flicker, smooth sweep.
- **Radar render (`drawRadar`):**
  - 3 concentric range rings (at ⅓, ⅔, 3⁄3 of `rangeKm`), faint crosshair, "N" tick at top.
  - Rotating sweep wedge (gradient trailing edge), ~1 revolution / 4 s, advanced
    by elapsed `millis` each frame (decorative; independent of poll cadence).
  - Blips at `polarToXY(bearingDeg(...), distKm, rangeKm, 120,120, ~96)`.
  - Nearest aircraft drawn brighter/larger with its callsign label.
  - Center dot = observer.
  - Stale marker when the last poll failed; "нет бортов" centered when list empty.
- **Detail render (`drawDetail`):** carousel card for `flights[index]`:
  large callsign, `type · arrow compassPoint`, big distance, altitude/speed row,
  page-position dots.
- **Touch (`cst816s` mini-driver):** minimal I2C read of the gesture register,
  exposing `NONE / CLICK / SWIPE_LEFT / SWIPE_RIGHT / SWIPE_DOWN`. INT on GPIO5.

### Component 3 — state machine

Two states, evaluated each loop with non-blocking timers.

- **RADAR**
  - `CLICK` → `DETAIL`, `index = 0`.
  - Continues animating sweep and updating blips on each poll.
- **DETAIL**
  - `SWIPE_LEFT` → `index = (index+1) % n`
  - `SWIPE_RIGHT` → `index = (index-1+n) % n`
  - `CLICK` or `SWIPE_DOWN` → `RADAR`
  - No touch for **15 s** → auto-return to `RADAR`.

Poll runs in both states. On refresh, the aircraft snapshot is swapped
atomically and `index` is clamped to the new list size (back to RADAR if list
becomes empty while in DETAIL).

### `platformio.ini`

- Replace `[env:esp32dev]` with `[env:esp32-s3]`:
  - ESP32-S3 board with PSRAM enabled.
  - `build_flags`: `-DARDUINO_USB_MODE=1 -DARDUINO_USB_CDC_ON_BOOT=1` (native USB serial),
    plus TFT_eSPI setup flags (`-DUSER_SETUP_LOADED`, `-DGC9A01_DRIVER`, pin defines,
    SPI frequency) so no library files are edited.
- `lib_deps`: `TFT_eSPI`, `ArduinoJson` (existing). CST816S handled by an
  in-repo mini-driver (single register read) — no extra dependency.
- `[env:native]` (Unity host tests) unchanged.

## Removed / out of scope

- Delete the 1602 / `LiquidCrystal` parallel-mode code introduced in `08135bf`.
- `formatLine1/2` in `flight_core.h` stay (their tests remain green) but are no
  longer used by firmware; may be removed in a later cleanup.
- No compass / heading-up, no IMU use, no battery indicator, no tap-on-blip
  selection, no settings UI.

## Error handling

- No WiFi / failed poll → render radar from last snapshot with a stale marker;
  empty list → centered "нет бортов".
- Touch I2C unresponsive → radar keeps animating, interaction simply disabled.

## Testing

- **Host (`[env:native]`, Unity):** extend `test/test_core` with:
  - `bearingDeg` against known coordinate pairs (cardinal directions, wrap-around).
  - `polarToXY` clamping (distance > range), center at zero distance, north-up sign.
  - `compassPoint` rose boundaries.
  - detail field formatters (ground, NaN, clamps).
  - Existing 17 tests untouched and still green.
- **On-device (manual):** blips appear at plausible positions; sweep rotates
  smoothly; tap → detail; swipes page through flights with wrap; auto-return
  after 15 s; stale marker when WiFi/API drops.

## Notes / known traps (from prior build)

- Flashing: ESP32-S3 has native USB CDC; auto-reset usually works, but keep the
  manual **BOOT-hold** fallback from the prior board in mind if upload fails.
- Headless serial monitor workaround (pyserial via PlatformIO python) may still
  be needed on macOS.
- See `reference_esp32-platformio-traps` (memory) for the HTTPS-301 and monitor traps.
