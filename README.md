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
  distance. Each blip is **colored by altitude band** (ground/unknown grey,
  <3k ft red, 3–10k orange, 10–25k yellow-green, 25–40k cyan, >40k blue) and
  carries a short **heading vector** along its true track. The nearest aircraft
  gets a **white ring + callsign label**. Aircraft squawking an **emergency
  code** (7500/7600/7700) blink red with an `EMERGENCY <code>` banner. A red dot
  appears top-right if the last poll failed. **Range presets: 25 / 50 / 100 km**
  (default 50); swipe up = zoom in, swipe down = zoom out, clamped at the ends.
  A **range readout** shows the current value top-center under "N". Aircraft
  beyond the display range but still within reception render as **small grey dots
  on the rim** at their bearing. The selected range persists across reboots.
- **Detail view** (tap to open) — one flight at a time: callsign, type +
  compass direction, distance, altitude and speed. Below those: **Registration**,
  **Operator** (3-letter airline ICAO derived from the callsign), and **Route**
  (origin → dest, e.g. `EGLL > KJFK` — from the BLE packet when present, else a
  lazy cached hexdb.io lookup on Wi-Fi). Page dots navigate between aircraft.
  **Swipe up** (Wi-Fi only) opens the **Photo view** for the selected aircraft.
- **Photo view** (swipe up from detail, Wi-Fi only) — fetches a real photo from
  [planespotters.net](https://planespotters.net) by registration (falls back to
  hex), JPEG-decodes it into the 240×240 round display (scale + center-crop via
  `photo_core.h`), and stores up to 8 photos in a PSRAM LRU cache across detail
  page switches. A `(c) photographer / planespotters.net` attribution line overlays
  the bottom. Any touch exits back to the detail view; idle for 15 s returns to
  the radar. Shows **"No Wi-Fi"** when the device is in BLE-only mode.
- **Source indicator** (bottom-center): green **W** = Wi-Fi live, red **W** =
  Wi-Fi up but the API poll is failing, cyan **B** = data coming over BLE from a
  phone, red **NO LINK** = no fresh data from either source.

## Controls

| Gesture | Action |
|---------|--------|
| Tap (on radar) | Open detail of the nearest flight |
| Swipe up (on radar) | Zoom in — switch to next smaller range preset |
| Swipe down (on radar) | Zoom out — switch to next larger range preset |
| Long-press (on radar) | Open the Wi-Fi setup captive portal |
| Swipe left / right (in detail) | Next / previous aircraft |
| Swipe up (in detail) | Open photo view for the selected aircraft (Wi-Fi only) |
| Tap or swipe down (in detail) | Back to radar |
| No touch for 15 s (in detail) | Auto-return to radar |
| Any touch (in photo) | Back to detail |
| No touch for 15 s (in photo) | Auto-return to radar |

## Setup

1. Install PlatformIO Core: `brew install platformio`
2. On first boot the device raises a **`FlightRadar-Setup`** Wi-Fi access point.
   Connect from a phone browser — the captive portal lets you pick the network,
   enter the password, and set your observer lat/lon without re-flashing. A
   **long-press** on the radar reopens the portal on demand. If not configured
   within 180 s the device boots offline (BLE fallback).

   `cp src/config.example.h src/config.h` — the values in `config.h` act as a
   **seed**: `WIFI_SSID`/`WIFI_PASS` are used only on a fresh device with empty
   NVS; `MY_LAT`/`MY_LON` are defaults until overridden via the portal.
   `RADIUS_NM` is **legacy/unused** — the poll radius is derived automatically
   from the widest range preset (100 km / 54 NM). `config.h` is gitignored —
   your credentials never reach the repo.

   Alternatively, Wi-Fi credentials can be sent **over BLE from the companion
   app** ("Configure device Wi-Fi" section) without touching the portal.
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
| `src/render_core.h` | Bearing, polar→screen projection, heading vectors, altitude band, emergency-squawk test, compass points, field formatting; route/operator helpers (`parseHexdbRoute`, `airlineCode`); range helpers (`kRangePresets`, `clampRangeIndex`, `isOnRim`, `queryRadiusNm`) (host-tested) |
| `src/ble_core.h` | BLE wire protocol + `parseBlePacket` (host-tested) |
| `src/coord_core.h` | `parseLatLon` — portal coordinate validation (host-tested) |
| `src/wifi_config_core.h` | `parseWifiConfig` — BLE Wi-Fi provisioning packet (host-tested) |
| `src/cst816s.h` | Minimal CST816S touch gesture driver |
| `src/photo_core.h` | `parsePlanespottersPhoto` → `PsPhoto{ok,url,photographer}`, `pickJpegScale(srcW,srcH)` → divisor, `cropOffset(scaledDim)` → offset; also the shared `PhotoResult` type (host-tested) |
| `src/flight_ticker.ino` | Wi-Fi/HTTP, NimBLE peripheral, TFT_eSPI sprite rendering, touch + radar/detail/photo state machine |

`pio test -e native -f test_core` runs the unit tests (60 cases, including the
BLE packet parser and Wi-Fi scan packet parser). The companion app has its own
Flutter unit tests: `cd companion && flutter test` (53 cases).

## BLE fallback (optional)

Wi-Fi is the primary data path. As a fallback, the device also runs a BLE
peripheral (`src/ble_core.h` + the NimBLE setup in the `.ino`): a phone
companion can write one compact binary packet of nearby aircraft, and the radar
re-centers on the packet's GPS and plots them. BLE data is used **only when
Wi-Fi is down** and the last packet is still fresh (≤ `BLE_FRESHNESS_MS`,
default 30 s); after that the screen shows **NO LINK**. The wire format
(v3, header + up to 10 × 48-byte records) and GATT UUIDs are documented in
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md). There are **four** GATT
characteristics: `f1a90002` (aircraft ingest, WRITE), `f1a90003` (Wi-Fi
config, WRITE + NOTIFY — used by the BLE provisioning path above),
`f1a90004` (Wi-Fi network scan, WRITE + NOTIFY — app writes a scan request,
device scans asynchronously and notifies one record per discovered network),
and `f1a90005` (photo transfer, WRITE + NOTIFY — see below).

**Photos over BLE.** The PHOTO view works on the BLE path too: a swipe-up
NOTIFYs a photo request (`f1a90005`) for the selected aircraft; the companion app
fetches the wsrv.nl-proxied, extra-compressed 240×240 baseline JPEG and WRITEs it
back in chunks, which the device decodes on-device via the shared
`decodeJpegToCache` path (same as the Wi-Fi photo flow). No photo / phone offline
shows "No photo".

> **Phone companion app** — `companion/` is a Flutter app (Android **and** iOS,
> hardware-verified). It is both a **viewer** and a feeder: its home screen shows
> a live list of nearby aircraft cards (photo from planespotters.net, type,
> distance, route, registration, EMG/MIL badges) and fires a **local
> notification** when an emergency-squawk or military aircraft appears (works in
> the background). When Wi-Fi is down it also feeds aircraft to the device over
> BLE (the production sender for this fallback). It can also provision the
> device's Wi-Fi credentials over BLE from its "Configure device Wi-Fi" section
> — including a **scan-to-pick** flow: tap the Wi-Fi scan button next to the SSID
> field, the device scans and streams nearby networks back over BLE, and a picker
> sheet lets you tap one to fill the SSID automatically. Tap any aircraft card to
> open a **live detail sheet** with a full field grid (altitude, speed, track,
> squawk, route, distance, registration, ICAO24, position, on-ground), an OSM
> mini-map with a track-rotated aircraft marker and observer dot, EMG/MIL badges,
> and a "Signal lost" banner that retains the last known data.
> `scripts/ble_send.py` remains a laptop smoke-test harness.
> See [companion/README.md](companion/README.md).

Test it against a flashed device from your laptop:

```bash
pip install bleak
python3 scripts/ble_send.py   # one sample v3 3-aircraft packet near Lisbon
                              # (incl. a 7700 emergency + an on-ground aircraft)
```

The device advertises as `FlightRadar`. On macOS, grant your terminal Bluetooth
permission (System Settings → Privacy). The 30 s freshness window expires fast,
so trigger the send right before you look at the screen, or widen
`BLE_FRESHNESS_MS` while testing.

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
