# Flight Radar — ESP32-S3-Touch-LCD-1.28 (project brief)

Live aircraft radar on a round touch display. The firmware polls airplanes.live
for nearby aircraft and plots them North-up by bearing + distance; a tap opens a
detail carousel of the nearest flights (swipe to page). Wi-Fi is primary; an
optional BLE path lets a phone feed aircraft as a fallback (see below).

Full docs: [README.md](README.md), [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md),
[docs/HARDWARE.md](docs/HARDWARE.md).

## Hardware

**Waveshare ESP32-S3-Touch-LCD-1.28**: ESP32-S3R2 (2 MB PSRAM), round GC9A01
240×240 LCD (SPI), CST816S touch (I2C), QMI8658 IMU (unused). USB-C, native USB.
Integrated board — no wiring; flash over USB-C.

## Toolchain (macOS): PlatformIO

`brew install platformio`. Everything from the terminal, no Arduino IDE.
- Tests (host, no hardware): `pio test -e native -f test_core` (43 cases)
- Companion app tests: `cd companion && flutter test`
- Build: `pio run -e esp32-s3`
- Flash: `pio run -e esp32-s3 -t upload` (native USB auto-resets; no BOOT hold)
- BLE smoke test: `pip install bleak; python3 scripts/ble_send.py` → sends one
  sample v2 packet (incl. a 7700 emergency) to a flashed device (advertises as
  `FlightRadar`)

## Code layout

Pure, host-testable core + thin Arduino layer:
- `src/flight_core.h` — parse (incl. `track`/`squawk`) / haversine / sort (Arduino-free)
- `src/render_core.h` — bearing / polar projection / `vectorEnd` / `altBand` / `isEmergencySquawk` / compass / formatting (Arduino-free, tested)
- `src/cst816s.h` — CST816S touch driver
- `src/ble_core.h` — BLE wire protocol (v2) + `parseBlePacket` (Arduino-free, tested)
- `src/flight_ticker.ino` — Wi-Fi/HTTP + NimBLE peripheral + TFT_eSPI sprite + radar/detail state machine
- `scripts/ble_send.py` — host BLE smoke-test sender (bleak), emits v2 packets
- `companion/` — Flutter phone app (Android + iOS): polls airplanes.live at the phone's GPS and feeds aircraft to the device over BLE when Wi-Fi is down (the fallback's phone side)

Config + secrets in `src/config.h` (copy from `config.example.h`; gitignored).

## Gotchas (see docs/HARDWARE.md for detail)

- `-DUSE_FSPI_PORT` is **required** or TFT_eSPI boot-loops on the S3.
- Touch is read on a falling-edge INT ISR + 300 ms debounce (CST816S sleeps when
  idle and fires many INT events per touch).
- Radar is North-up only (no magnetometer on the board).
- airplanes.live forces HTTPS → `WiFiClientSecure` + `setInsecure()`; poll ≤ 1/s
  (firmware uses 15 s). `RADIUS_NM` is nautical miles; on-screen distances in km.
- BLE is fallback-only: used when Wi-Fi is down AND last packet ≤ `BLE_FRESHNESS_MS`
  (30 s) old, else `NO LINK`. Source indicator (bottom-center): green W / red W /
  cyan B / red NO LINK. Write callback only buffers + flags; `loop()` parses (no race).
- BLE wire = v2: 12 B header + ≤ **15** × 32 B records (15 caps it at 492 B for a
  single ATT write); records carry `track`+`squawk`; display still caps `MAX_AIRCRAFT` (10).
- `HIDE_GROUND_AIRCRAFT` (default 1) drops on-ground aircraft from radar + list on both paths.
- NimBLE pinned to `^1.4.1` (1.x single-arg `onWrite`); 2.x changed the signature.
  NimBLE + Wi-Fi/TLS + 115 KB sprite all coexist in SRAM (verified on device).
- BLE freshness window is short (30 s) — when testing, send right before observing
  or widen `BLE_FRESHNESS_MS`.

## Ideas / backlog

Switch to local reception (RTL-SDR + dump1090) instead of the API, Cyrillic font
for labels.

**Done:** BLE phone-fallback data path (v2 wire protocol + parser + NimBLE
peripheral + source arbitration). **Phone companion app shipped** — Flutter
(Android + iOS, hardware-verified) in `companion/`; `scripts/ble_send.py` is now
just the laptop smoke-test sender. Radar enrichment shipped: altitude-band blip
colors, per-aircraft heading vectors, emergency-squawk (7500/7600/7700) blink +
banner, and the `HIDE_GROUND_AIRCRAFT` filter.
