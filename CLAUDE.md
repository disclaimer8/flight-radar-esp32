# Flight Radar — ESP32-S3-Touch-LCD-1.28 (project brief)

Live aircraft radar on a round touch display. The firmware polls airplanes.live
for nearby aircraft and plots them North-up by bearing + distance; a tap opens a
detail carousel of the nearest flights (swipe to page).

Full docs: [README.md](README.md), [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md),
[docs/HARDWARE.md](docs/HARDWARE.md).

## Hardware

**Waveshare ESP32-S3-Touch-LCD-1.28**: ESP32-S3R2 (2 MB PSRAM), round GC9A01
240×240 LCD (SPI), CST816S touch (I2C), QMI8658 IMU (unused). USB-C, native USB.
Integrated board — no wiring; flash over USB-C.

## Toolchain (macOS): PlatformIO

`brew install platformio`. Everything from the terminal, no Arduino IDE.
- Tests (host, no hardware): `pio test -e native -f test_core` (29 cases)
- Build: `pio run -e esp32-s3`
- Flash: `pio run -e esp32-s3 -t upload` (native USB auto-resets; no BOOT hold)

## Code layout

Pure, host-testable core + thin Arduino layer:
- `src/flight_core.h` — parse / haversine / sort (Arduino-free)
- `src/render_core.h` — bearing / polar projection / compass / formatting (Arduino-free, tested)
- `src/cst816s.h` — CST816S touch driver
- `src/flight_ticker.ino` — Wi-Fi/HTTP + TFT_eSPI sprite + radar/detail state machine

Config + secrets in `src/config.h` (copy from `config.example.h`; gitignored).

## Gotchas (see docs/HARDWARE.md for detail)

- `-DUSE_FSPI_PORT` is **required** or TFT_eSPI boot-loops on the S3.
- Touch is read on a falling-edge INT ISR + 300 ms debounce (CST816S sleeps when
  idle and fires many INT events per touch).
- Radar is North-up only (no magnetometer on the board).
- airplanes.live forces HTTPS → `WiFiClientSecure` + `setInsecure()`; poll ≤ 1/s
  (firmware uses 15 s). `RADIUS_NM` is nautical miles; on-screen distances in km.

## Ideas / backlog

Altitude filter, per-aircraft track vector, switch to local reception
(RTL-SDR + dump1090) instead of the API, Cyrillic font for labels.
