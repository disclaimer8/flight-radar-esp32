# Flight Radar — ESP32-S3-Touch-LCD-1.28 (project brief)

Live aircraft radar on a round touch display. The firmware polls airplanes.live
for nearby aircraft and plots them North-up by bearing + distance; touch range
presets (25 / 50 / 100 km) zoom the view and out-of-range traffic appears as rim
dots. A tap opens a detail carousel showing callsign, altitude, speed, heading,
plus registration, operator (airline ICAO), and route (origin→dest). Wi-Fi is
provisioned via a WiFiManager captive portal (`FlightRadar-Setup` AP) or over BLE
from the companion app; an optional BLE fallback path lets a phone feed aircraft
when Wi-Fi is down (see below).

Full docs: [README.md](README.md), [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md),
[docs/HARDWARE.md](docs/HARDWARE.md).

## Hardware

**Waveshare ESP32-S3-Touch-LCD-1.28**: ESP32-S3R2 (2 MB PSRAM), round GC9A01
240×240 LCD (SPI), CST816S touch (I2C), QMI8658 IMU (unused). USB-C, native USB.
Integrated board — no wiring; flash over USB-C.

## Toolchain (macOS): PlatformIO

`brew install platformio`. Everything from the terminal, no Arduino IDE.
- Tests (host, no hardware): `pio test -e native -f test_core` (52 cases)
- Companion app tests: `cd companion && flutter test` (39 cases)
- Build: `pio run -e esp32-s3`
- Flash: `pio run -e esp32-s3 -t upload` (native USB auto-resets; no BOOT hold)
- BLE smoke test: `pip install bleak; python3 scripts/ble_send.py` → sends one
  sample v3 packet (incl. a 7700 emergency) to a flashed device (advertises as
  `FlightRadar`)

## Code layout

Pure, host-testable core + thin Arduino layer:
- `src/flight_core.h` — parse (incl. `track`/`squawk`/`registration`/`origin`/`dest`) / haversine / sort (Arduino-free)
- `src/render_core.h` — bearing / polar projection / `vectorEnd` / `altBand` / `isEmergencySquawk` / compass / formatting + route/operator helpers (`parseHexdbRoute`, `airlineCode`) + range-zoom helpers (`kRangePresets` / `clampRangeIndex` / `isOnRim` / `queryRadiusNm`) (Arduino-free, tested)
- `src/cst816s.h` — CST816S touch driver
- `src/ble_core.h` — BLE wire protocol (v3) + `parseBlePacket` (Arduino-free, tested)
- `src/coord_core.h` — `parseLatLon` (captive-portal coordinate validation, host-tested)
- `src/wifi_config_core.h` — `parseWifiConfig` (BLE Wi-Fi provisioning packet, host-tested)
- `src/flight_ticker.ino` — Wi-Fi/HTTP + NimBLE peripheral + TFT_eSPI sprite + radar/detail state machine
- `scripts/ble_send.py` — host BLE smoke-test sender (bleak), emits v3 packets
- `companion/` — Flutter phone app (Android + iOS): BLE feeder (polls airplanes.live at the phone's GPS, feeds aircraft to the device when Wi-Fi is down) + live aircraft viewer (planespotters photos, route, EMG/MIL badges, emergency/military local notifications) + BLE Wi-Fi provisioning section

Config + secrets in `src/config.h` (copy from `config.example.h`; gitignored).

## Gotchas (see docs/HARDWARE.md for detail)

- `-DUSE_FSPI_PORT` is **required** or TFT_eSPI boot-loops on the S3.
- Touch is read on a falling-edge INT ISR + 300 ms debounce (CST816S sleeps when
  idle and fires many INT events per touch).
- Radar is North-up only (no magnetometer on the board).
- airplanes.live forces HTTPS → `WiFiClientSecure` + `setInsecure()`; poll ≤ 1/s
  (firmware uses 15 s). `RADIUS_NM` in `config.h` is legacy/unused — poll radius is
  now derived from the widest range preset via `queryRadiusNm`; on-screen distances in km.
- Wi-Fi setup uses **WiFiManager** (`tzapu/WiFiManager@^2.0.17`) — opens a
  `FlightRadar-Setup` captive-portal AP that collects SSID/password + observer
  lat/lon; long-press reopens it at runtime. `config.h` `WIFI_SSID`/`WIFI_PASS` are
  a seed credential and `MY_LAT`/`MY_LON` are defaults; runtime values live in
  `g_obsLat`/`g_obsLon` and are persisted in NVS (namespace `"radar"`).
- A **second GATT characteristic** (`f1a90003`, WRITE+NOTIFY) lets the companion app
  provision Wi-Fi credentials over BLE, using the `parseWifiConfig` wire format in
  `src/wifi_config_core.h`.
- Range presets (25/50/100 km) are stored in NVS (`rangeIdx`); swipe up/down zooms,
  long-press reopens the captive portal. Out-of-range traffic appears as rim dots.
- BLE is fallback-only: used when Wi-Fi is down AND last packet ≤ `BLE_FRESHNESS_MS`
  (30 s) old, else `NO LINK`. Source indicator (bottom-center): green W / red W /
  cyan B / red NO LINK. Write callback only buffers + flags; `loop()` parses (no race).
- BLE wire = **v3**: 12 B header + ≤ **10** × **48 B** records (caps at 492 B for one
  ATT write); records carry `track`/`squawk` + `registration` (8 B) + `origin`/`dest`
  ICAO (4 B each); display caps `MAX_AIRCRAFT` (10).
- `HIDE_GROUND_AIRCRAFT` (default 1) drops on-ground aircraft from radar + list on both paths.
- NimBLE pinned to `^1.4.1` (1.x single-arg `onWrite`); 2.x changed the signature.
  NimBLE + Wi-Fi/TLS + 115 KB sprite all coexist in SRAM (verified on device).
- BLE freshness window is short (30 s) — when testing, send right before observing
  or widen `BLE_FRESHNESS_MS`.

## Ideas / backlog

Switch to local reception (RTL-SDR + dump1090) instead of the API, Cyrillic font
for labels.

**Done:** BLE phone-fallback data path (v3 wire protocol + parser + NimBLE
peripheral + source arbitration). Phone companion app shipped — Flutter (Android +
iOS, hardware-verified) in `companion/`; `scripts/ble_send.py` is the laptop
smoke-test sender. Radar enrichment: altitude-band blip colors, per-aircraft
heading vectors, emergency-squawk (7500/7600/7700) blink + banner,
`HIDE_GROUND_AIRCRAFT` filter. **Detail enrichment** — tap carousel now shows
registration, operator (airline ICAO), and route (origin→dest). **Radar range
presets** (25/50/100 km, NVS-persisted, swipe up/down) + out-of-range rim dots.
**WiFiManager captive portal** (`FlightRadar-Setup`) — provisions Wi-Fi creds +
observer lat/lon; long-press reopens; new dep `tzapu/WiFiManager`. **BLE Wi-Fi
provisioning** — second GATT characteristic (`f1a90003`) + `wifi_config_core.h`
wire format lets the companion app set Wi-Fi over BLE. **Companion viewer** —
app now also shows a live aircraft list with planespotters photos, route, EMG/MIL
badges, and fires emergency/military local notifications.

See README / docs/ARCHITECTURE.md / docs/HARDWARE.md for full detail.
