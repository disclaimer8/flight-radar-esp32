# Flight Ticker — Design Spec

**Date:** 2026-05-30
**Status:** Approved
**Target:** ESP32 DevKit (`esp32dev`) + LCD 1602 (HD44780 via PCF8574 I2C)

## Goal

Firmware that polls the airplanes.live REST API over Wi-Fi for aircraft near a
fixed observer location, computes distance to each, and rotates the nearest N
aircraft across a 16×2 character LCD:

- **Line 1:** callsign + distance (km)
- **Line 2:** aircraft type + altitude (m) + ground speed (km/h)

Built from scratch per the project brief (`CLAUDE.md`). Toolchain is PlatformIO
on macOS, no Arduino IDE.

## Architecture: thin firmware + pure core

The "thinking" logic lives in `flight_core.h` with **no Arduino dependencies**
(only ArduinoJson, which compiles on the host too). The `.ino` is a thin layer:
Wi-Fi, HTTP, LCD, timers.

```
flight-ticker/
├── platformio.ini
├── .gitignore              # config.h, .pio/
├── CLAUDE.md               # brief, used as project context
├── README.md               # quick start + troubleshooting
├── src/
│   ├── flight_ticker.ino   # hardware: WiFi/HTTP/LCD/loop
│   ├── flight_core.h       # PURE logic: parse → distance → sort → format
│   ├── config.h            # secrets (gitignored)
│   └── config.example.h    # template, committed
└── test/
    └── test_core/          # native unit tests for the core
```

Rationale: 16-char formatting and unit conversion are bug-prone and painful to
debug on-device. Isolating the core lets us run TDD tests on the Mac
(`pio test -e native`) before the first flash. The `.ino` stays simple.

### Unit boundaries

- **`flight_core.h`** — pure, host-compilable. Responsibilities:
  - `parseAircraft(json, maxN)` → vector of `Aircraft{callsign, type, altFt, gsKt, lat, lon}` using an ArduinoJson filter (only `flight, t, alt_baro, gs, lat, lon`).
  - `haversineKm(lat1, lon1, lat2, lon2)` → distance in km.
  - `sortByDistance(...)` and truncate to top-N.
  - `formatLine1(ac, distKm)` / `formatLine2(ac)` → exactly-16-char strings.
  - Unit helpers: `ftToM`, `ktToKmh`.
  - Inputs: raw JSON string + observer lat/lon. Outputs: formatted display
    strings. Depends only on ArduinoJson + std. No `WiFi`, no `LiquidCrystal`.
- **`flight_ticker.ino`** — hardware/timing. Owns Wi-Fi connect/reconnect, HTTP
  GET, the two `millis()` timers, LCD driver, and the boot-time I2C scan. Calls
  into `flight_core.h`. Knows nothing about distance math or string formatting.

## Data flow (non-blocking loop)

Two independent `millis()` timers, no `delay()`:

1. **Poll** every `POLL_INTERVAL_MS` (default 15000):
   `GET http://api.airplanes.live/v2/point/{MY_LAT}/{MY_LON}/{RADIUS_NM}`
   - Plain HTTP (no TLS overhead on ESP32).
   - Sends a `User-Agent` header (airplanes.live requests one).
   - Rate limit is 1 req/s; 15s poll is well within it.
2. **Parse** with ArduinoJson v7 using a field filter → compute haversine
   distance from home → sort ascending → cache top-N (default 5).
3. **Cycle** display every `CYCLE_INTERVAL_MS` (default 5000): show the next
   cached aircraft. The cached list keeps rotating between polls, so the screen
   stays alive even when the network is quiet.

## LCD layout (16×2) and units

```
Line 1:  DLH4AB      12km     ← callsign (≤8) + distance, km
Line 2:  A320 10668m  840     ← type + altitude (m) + ground speed (km/h)
```

Conversions: `alt_baro` ft→m (×0.3048), `gs` kt→km/h (×1.852), distance via
haversine directly in km. Long callsigns/types are truncated to fit width.
`alt_baro: "ground"` renders as `GND`.

## Edge cases

- **Empty radius** → `No aircraft` / `in range NNkm` (LCD text kept English per
  brief).
- **Wi-Fi dropped** → `WiFi...` + auto-reconnect.
- **HTTP non-200 / timeout** → keep the last cache, do not clear the screen
  (small "stale" indicator).
- **Boot diagnostic** → print an I2C scan to Serial at startup (finds `0x27` /
  `0x3F`), so no separate scanner sketch is needed for contrast/address debug.

## Toolchain and dependencies

`platformio.ini` per brief (`esp32dev`, arduino framework, `monitor_speed=115200`):

- `bblanchon/ArduinoJson@^7.0.0`
- `marcoschwartz/LiquidCrystal_I2C@^1.1.4`
- Parallel (non-PCF8574) LCD mode left as a commented alternative.

Plus `[env:native]` using the Unity test framework for `test/test_core/`.

## Configuration

`src/config.h` (gitignored) holds secrets/tunables; `src/config.example.h` is
the committed template:

- `WIFI_SSID`, `WIFI_PASS` (2.4 GHz only — ESP32 has no 5 GHz radio)
- `MY_LAT`, `MY_LON` (decimal degrees)
- `RADIUS_NM` (default 30)
- Tunables: `POLL_INTERVAL_MS=15000`, `CYCLE_INTERVAL_MS=5000`, `MAX_AIRCRAFT=5`,
  `LCD_ADDR=0x27`, `LCD_SDA=21`, `LCD_SCL=22`

## Testing

Native unit tests (`pio test -e native`) cover the pure core:

- `haversineKm` against known coordinate pairs.
- `ftToM` / `ktToKmh` conversions.
- `parseAircraft` on a captured sample JSON response (correct fields, count,
  filter behavior, `"ground"` altitude handling).
- `formatLine1` / `formatLine2` produce exactly 16 chars, truncate long
  callsigns/types, and render `GND`.

On-device verification is manual (flash, watch serial + LCD) and documented in
the README troubleshooting section.

## Out of scope (future, per brief)

Azimuth "where to look", altitude filtering for directly-overhead aircraft,
local reception via RTL-SDR + dump1090 instead of the API.
