# Flight Radar (ESP32-S3)

A live aircraft radar on a round touch display. The ESP32 polls
[airplanes.live](https://airplanes.live) for aircraft near your location and
plots them on a North-up radar by bearing and distance. Tap the screen to open
a detail carousel of the nearest flights.

Hardware: **Waveshare ESP32-S3-Touch-LCD-1.28** (round GC9A01 240×240 LCD,
CST816S capacitive touch, ESP32-S3R2).

## What it shows

- **Radar view** — concentric range rings, a rotating sweep, North at the top,
  the observer at the center, and a blip per aircraft placed by bearing +
  distance. The nearest aircraft is highlighted with its callsign. A red dot
  appears top-right if the last poll failed.
- **Detail view** (tap to open) — one flight at a time: callsign, type +
  compass direction, distance, altitude and speed, with page dots.

## Controls

| Gesture | Action |
|---------|--------|
| Tap (on radar) | Open detail of the nearest flight |
| Swipe left / right (in detail) | Next / previous aircraft |
| Tap or swipe down (in detail) | Back to radar |
| No touch for 15 s (in detail) | Auto-return to radar |

## Setup

1. Install PlatformIO Core: `brew install platformio`
2. `cp src/config.example.h src/config.h` and fill in your Wi-Fi (2.4 GHz only),
   latitude/longitude, and `RADIUS_NM` (search radius in nautical miles).
   `config.h` is gitignored — your credentials never reach the repo.
3. Run the host tests: `pio test -e native -f test_core`
4. Build: `pio run -e esp32-s3`
5. Flash: `pio run -e esp32-s3 -t upload` (the S3's native USB auto-resets; no
   BOOT-button hold needed).
6. Monitor (optional): `pio device monitor -b 115200`.

## How it's built

The aircraft logic and rendering math are pure and Arduino-free, so they run as
host unit tests under the `native` environment — no hardware needed.

| File | Responsibility |
|------|----------------|
| `src/flight_core.h` | Poll parsing (ArduinoJson), haversine distance, sort by nearest |
| `src/render_core.h` | Bearing, polar→screen projection, compass points, field formatting (host-tested) |
| `src/cst816s.h` | Minimal CST816S touch gesture driver |
| `src/flight_ticker.ino` | Wi-Fi/HTTP, TFT_eSPI sprite rendering, touch + radar/detail state machine |

`pio test -e native -f test_core` runs the unit tests (29 cases).

## Documentation

- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — module breakdown, data flow,
  the radar/detail state machine, testing strategy, and how to extend it.
- [docs/HARDWARE.md](docs/HARDWARE.md) — board specs, pin map, build-flag
  rationale, flashing, and the bring-up gotchas (the SPI-port boot crash, the
  CST816S touch handling, native-USB serial, sprite memory).

## Pin map (ESP32-S3-Touch-LCD-1.28)

GC9A01 LCD over SPI: MOSI 11, SCLK 10, CS 9, DC 8, RST 14, backlight 2.
CST816S touch over I2C: SDA 6, SCL 7, INT 5, RST 13.
LCD pins are configured via `build_flags` in `platformio.ini`; touch pins in
`config.h`.

## Notes

- **North-up only.** The board's IMU has no magnetometer, so the radar can't
  rotate to physical heading. Top of screen = geographic North; the per-aircraft
  arrow is the bearing from your coordinates.
- **`-DUSE_FSPI_PORT` is required.** Without it, TFT_eSPI's default `SPI_PORT`
  on the S3 misresolves and the board boot-loops (StoreProhibited) on the first
  display command.
- **Touch is INT-driven.** The CST816S is read on a falling-edge interrupt with
  a short debounce — it emits many INT events per touch, so one physical tap maps
  to one action.
- airplanes.live forces HTTPS (Cloudflare 301); the firmware uses
  `WiFiClientSecure` with `setInsecure()` for this public read-only data.

## Data

Aircraft data from [airplanes.live](https://airplanes.live) (rate limit 1 req/s;
the firmware polls every 15 s).
