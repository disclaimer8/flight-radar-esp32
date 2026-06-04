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
  `registration` (tail number), `origin` (route origin ICAO), `dest` (route
  dest ICAO), lat/lon, and `distKm` (filled during parse).
- `parseNearest(json, myLat, myLon, maxN, hideGround)` — parses the
  airplanes.live response with an ArduinoJson filter (only the fields we use,
  including `track`, `squawk`, and `r` for registration), computes great-circle
  distance to each aircraft via `haversineKm`, sorts nearest-first, and caps to
  `maxN`. With `hideGround` set it drops on-ground aircraft before the sort/cap.
  Returns an empty vector on malformed JSON.
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
- `parseHexdbRoute(routeStr)` → `(origin, dest)` — splits an `"EGLL-KJFK"` style
  hexdb route string (first and last ICAO codes for multi-leg routes).
- `airlineCode(callsign)` → 3-letter airline ICAO prefix, or `""` for tail-number
  callsigns (all-digit or too short).
- **Range-zoom helpers**: `kRangePresets` (`{25.0, 50.0, 100.0}` km),
  `kRangeCount` (`3`), `clampRangeIndex(idx, delta, count)` (ladder, no wrap),
  `isOnRim(distKm, displayRangeKm)` (true when aircraft is beyond the display
  range → draw as grey rim dot), `queryRadiusNm(maxPresetKm)` (API poll radius
  in NM, used to build the airplanes.live URL).

Keeping these pure is what makes the project testable: see
`test/test_core/test_main.cpp` (56 cases — cardinal bearings, projection
clamping and off-axis geometry, compass rounding boundaries, formatter edge
cases, vector/altitude-band/emergency-squawk helpers, the parse/sort/distance
tests, the BLE packet parser, range-zoom helpers, route/airline-code helpers,
coord/wifi-config parsers, and wifi-scan request/record packet helpers).

### `src/coord_core.h` — coordinate validation (pure, Arduino-free, host-tested)
- `parseLatLon(latStr, lonStr, &lat, &lon)` — validates two C-string coordinates
  and, on success, writes `lat`/`lon` and returns `true`. Returns `false` if
  either string is empty, non-numeric, has trailing garbage, is out of range
  (lat `[-90, 90]`, lon `[-180, 180]`), or is `NaN`/`±Inf`. Used by the captive
  portal's save callback to reject garbage user input.

### `src/wifi_config_core.h` — BLE Wi-Fi provisioning packet (pure, Arduino-free, host-tested)
- `struct WifiConfig` — `ok`, `ssid`, `pass`.
- `parseWifiConfig(buf, len)` → `WifiConfig` — decodes the BLE provisioning
  packet written by the phone app: magic `0x57 0x43` (`"WC"`), version `1`,
  `ssidLen` (1–32) + ssid bytes, `passLen` (0–63) + pass bytes. Returns
  `ok = false` on wrong magic/version, zero or oversized SSID length, oversized
  pass length, or a truncated buffer.

### `src/ble_core.h` — BLE wire protocol + parser (pure, Arduino-free, host-tested)
- `parseBlePacket(buf, len, maxN, hideGround)` → `BlePacket` — decodes one
  binary packet (v3 format below), fills each aircraft's `distKm` via
  `haversineKm` from the packet's center, sorts nearest-first, and caps to
  `maxN`. With `hideGround` set it drops on-ground aircraft. Returns
  `ok = false` on any validation failure (bad magic/version, count over the
  cap, or a length that doesn't match `header + count·record`).
- Like `flight_core.h`, this is pure and runs under `native` (valid decode, bad
  magic, bad version, count overflow, length mismatch, flag handling including
  `track`/`squawk`, hide-ground, cap-to-maxN, and the v3 registration/route
  fields).

### `src/cst816s.h` — touch driver (Arduino)
Minimal I2C driver for the CST816S capacitive controller (address `0x15`). Reads
the gesture register (`0x01`) and exposes a `TouchGesture` enum
(`TG_CLICK`, `TG_LEFT`, `TG_RIGHT`, `TG_DOWN`, `TG_UP`, `TG_LONG`, …). Returns
`TG_NONE` on any I2C error, so a missing/asleep chip never blocks rendering.

### `src/flight_ticker.ino` — firmware
- **Networking** — `connectWifi()` (via `WiFiManager.autoConnect`; see "Wi-Fi
  provisioning" below) and `pollApi()` (HTTPS via `WiFiClientSecure` +
  `setInsecure()`, because airplanes.live 301-redirects HTTP). Polled on a
  non-blocking 15 s `millis` timer in both views. The poll URL uses
  `queryRadiusNm(kRangePresets[kRangeCount - 1])` (100 km / 54 NM) as the fixed
  reception radius; `RADIUS_NM` in `config.h` is legacy and unused at runtime.
- **Rendering** — a full-screen 240×240 `TFT_eSprite` is the framebuffer; every
  frame is drawn into the sprite and `pushSprite`'d in one shot (no flicker).
  `drawRadar()` and `drawDetail()` are the two renderers. `drawRadar()` colors
  each blip by `altBand()` (via the `kAltColors` table), draws a `vectorEnd()`
  heading line, rings + labels the nearest aircraft in white, and — for any
  `isEmergencySquawk()` aircraft — blinks it red and shows an `EMERGENCY <code>`
  banner. Aircraft beyond the current display range (`isOnRim`) are drawn as
  small grey rim dots at the correct bearing on the outermost ring. A range
  readout (e.g. `50km`) draws top-center below the `N` label. `drawDetail()`
  shows callsign, type + compass direction + squawk, distance, altitude/speed,
  and a Reg / Op / Route block — registration from the `Aircraft` struct, airline
  code via `airlineCode(callsign)`, and route origin→dest from either the BLE
  packet or a lazy cached `hexdb.io` lookup.
- **Touch** — a falling-edge ISR latches INT events; `handleTouch()` reads the
  gesture once per event with a 300 ms debounce (see Hardware notes for why).
- **BLE ingest** — a NimBLE peripheral (`IngestCallbacks::onWrite`) receives
  binary packets from a phone; the callback only buffers the bytes and sets a
  flag, while `loop()` does the parse. See "BLE data path" below.
- **BLE Wi-Fi provisioning** — `WifiConfigCallbacks::onWrite` buffers a Wi-Fi
  credentials packet into `g_wifiCfgBuf`; `loop()` calls `applyWifiConfig()`,
  which parses, connects (up to ~12 s), and notifies status back on the
  wifi-config characteristic. See "Wi-Fi provisioning" below.
- **State machine** — see below.

## Data flow per poll

1. `pollApi()` requests `/v2/point/{lat}/{lon}/{radius_nm}` where `lat`/`lon`
   are `g_obsLat`/`g_obsLon` (runtime globals from NVS or config.h default) and
   `radius_nm` = `queryRadiusNm(100.0)` = 54 NM.
2. `parseNearest()` turns the JSON into a sorted, capped `vector<Aircraft>`
   stored in `g_cache` (cap `RADAR_PLOT_CAP` = 24, to keep distant aircraft
   available as rim dots); `g_idx` (the detail-carousel cursor) is clamped to
   the new size.
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
- Aircraft-ingest characteristic (`WRITE | WRITE_NR`):
  `f1a90002-7e1d-4c2a-9b3f-1a2b3c4d5e6f`
- Wi-Fi-config characteristic (`WRITE | WRITE_NR | NOTIFY`):
  `f1a90003-7e1d-4c2a-9b3f-1a2b3c4d5e6f`
- Wi-Fi-scan characteristic (`WRITE | NOTIFY`):
  `f1a90004-7e1d-4c2a-9b3f-1a2b3c4d5e6f`

**Wire format** (v3, little-endian; both ESP32 and host are LE). A 12-byte
header followed by `count` × 48-byte records:

| Header (12 B) | Field | Bytes |
|---|---|---|
| `0` | magic `0x46 0x52` (`'F' 'R'`) | 2 |
| `2` | version (`3`) | 1 |
| `3` | count (uint8, ≤ 10) | 1 |
| `4` | center lat (float32) | 4 |
| `8` | center lon (float32) | 4 |

| Record (48 B) | Field | Bytes |
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
| `32` | registration, ASCII, space-padded | 8 |
| `40` | origin ICAO, ASCII, space-padded | 4 |
| `44` | dest ICAO, ASCII, space-padded | 4 |

Flag bits: `GROUND = 0x01`, `ALT_VALID = 0x02`, `GS_VALID = 0x04`,
`TRACK_VALID = 0x08`, `SQUAWK_VALID = 0x10`. When a `*_VALID` bit is clear, the
parser stores `NaN` (or `0` for squawk) for that field and the UI shows it as
N/A. Up to **10** aircraft fit on the wire: 12 + 10·48 = **492 B** fits in one
ATT write at MTU 517. The display detail carousel pages all entries in `g_cache`
(which holds up to `RADAR_PLOT_CAP` = 24 by distance); `MAX_AIRCRAFT` (10) is
the BLE decode cap passed to `parseBlePacket`. `parseBlePacket` validates
magic/version/count/length, fills distance, sorts nearest-first, and caps — see
`src/ble_core.h`.

**Source arbitration** (in `loop()`, every frame): Wi-Fi wins whenever it's
connected; otherwise BLE is used if a packet arrived within `BLE_FRESHNESS_MS`
(default 30 s); otherwise the source is "none". A `g_bleLastRx != 0` guard keeps
BLE dormant until a real packet arrives (so an uninitialized timestamp at boot
isn't read as "fresh"). The bottom-center indicator reflects the live source:
green `W` (Wi-Fi), red `W` (Wi-Fi up, poll failing), cyan `B` (BLE), red
`NO LINK`.

**Concurrency.** The BLE write callbacks run in the NimBLE task. They do the
minimum: copy the bytes into a buffer and set a flag (`g_blePacketReady` or
`g_wifiCfgReady`). `loop()` (the same single thread that renders) does the
actual parse/apply and updates `g_cache` / the radar center / Wi-Fi config. So
the render path never races the BLE task — no locking needed, the flag is the
handoff. The same pattern applies to both characteristics.

## Wi-Fi provisioning

Wi-Fi credentials (and the observer location) are provisioned at runtime rather
than baked into the binary.

**Captive-portal path (tzapu/WiFiManager).** On first boot (or long-press),
`connectWifi()` calls `WiFiManager.autoConnect("FlightRadar-Setup")`. If stored
credentials connect, it returns immediately. Otherwise the device raises a
`FlightRadar-Setup` access point; a connected phone opens `192.168.4.1` and
fills in SSID, password, observer latitude, and observer longitude. The portal
saves credentials to flash and writes lat/lon to NVS (Preferences namespace
`"radar"`, keys `"lat"`/`"lon"`). The portal times out after 180 s and the
device boots offline (BLE fallback active). On any subsequent boot `autoConnect`
reconnects silently. A **long-press** (`TG_LONG` in RADAR state) reopens the
portal on demand via `startPortalOnDemand()`.

**BLE Wi-Fi-config path.** The companion app can also deliver credentials over
BLE. It writes a `"WC"` magic packet to the wifi-config characteristic
`f1a90003-…` (WRITE|WRITE_NR|NOTIFY); the firmware parses it with
`parseWifiConfig`, calls `WiFi.persistent(true)` + `WiFi.begin`, waits up to
~12 s, and notifies the result back on the same characteristic (1 byte code:
`0` = applying, `1` = connected + IP, `2` = failed + reason string). See
`src/wifi_config_core.h` for the packet format.

**BLE Wi-Fi-scan path (`f1a90004`).** The companion app writes a 3-byte scan
request to the wifi-scan characteristic `f1a90004-…` (WRITE|NOTIFY); the write
callback only sets a flag (`g_wifiScanReady`) — the actual `WiFi.scanNetworks`
call happens from `loop()` to avoid racing the NimBLE task. Results are sent
back as one NOTIFY per network (≤ 40 bytes each, fits any practical MTU); a
final notify with `total = 0` means no networks were found. The firmware
deduplicates by SSID (keeping the strongest RSSI), sorts descending by RSSI,
caps at 15 results, and drops hidden networks (empty SSID). Single-radio
caveat: scanning while STA-connected can briefly stall the in-flight HTTPS poll.

Wire formats (little-endian):

**Scan request** (app → device, 3 bytes):

| Offset | Field | Value |
|--------|-------|-------|
| `0–1` | magic `"WS"` | `0x57 0x53` |
| `2` | version | `1` |

**Scan record** (device → app, per NOTIFY):

| Offset | Field | Bytes |
|--------|-------|-------|
| `0–1` | magic `"WN"` (`0x57 0x4E`) | 2 |
| `2` | version (`1`) | 1 |
| `3` | total networks (uint8; `0` = none found) | 1 |
| `4` | index (0-based, uint8) | 1 |
| `5` | RSSI (int8, dBm) | 1 |
| `6` | secured (uint8; `1` = has password) | 1 |
| `7` | ssidLen (uint8) | 1 |
| `8…` | SSID bytes (ssidLen, ASCII) | ≤ 32 |

See `src/wifi_scan_core.h` for the packet helpers and the `ScanCollector` dedup logic.

**Observer location at runtime.** The observer lat/lon are the runtime globals
`g_obsLat`/`g_obsLon`. They are loaded from NVS at boot (with `config.h`
`MY_LAT`/`MY_LON` as the compile-time default on a fresh device), and updated by
both the portal save callback and any future provisioning path that calls
`saveLocation()`. All poll URLs and bearing math use `g_obsLat`/`g_obsLon`.

**`config.h` `WIFI_SSID`/`WIFI_PASS`** serve as a seed for a fresh device (no
NVS credentials yet) — they are written once to flash on the first `autoConnect`
call, after which the saved credentials take over. **`RADIUS_NM`** is
legacy/unused at runtime; the poll radius is `queryRadiusNm(100.0)` = 54 NM.

## Radar range presets and rim dots

The display range is a runtime choice from three presets (25 / 50 / 100 km)
selected by swipe up/down in RADAR view, persisted to NVS (key `"rangeIdx"`),
and restored on boot. `displayRangeKm()` returns `kRangePresets[g_rangeIdx]`.

`drawRadar()` projects every aircraft at the current display range. For aircraft
where `isOnRim(distKm, displayRangeKm)` is true (beyond the outer ring), a small
grey rim dot is drawn at the correct bearing — they are visible but de-emphasised.
In-range aircraft use the normal altitude-color blip + heading vector styling.
Emergency squawks blink red regardless of range.

The poll reception radius is always the widest preset (100 km → 54 NM), so the
firmware keeps up to `RADAR_PLOT_CAP` = 24 aircraft sorted by distance; the
detail carousel pages all of them regardless of the current zoom level.

## Phone companion

**`companion/`** — a Flutter app for **Android and iOS** (hardware-verified).

**Feeder role.** Polls airplanes.live around the phone's own GPS, encodes the
nearby aircraft as a v3 packet, and writes it to the device's GATT ingest
characteristic — the production sender for the BLE fallback path (when the device
has no Wi-Fi but the phone does). Built around a shared `GatewayEngine`
(poll → encode → BLE-write loop, in `lib/service/`) plus per-platform drivers so
it keeps running in the background: an Android foreground service
(`flutter_foreground_task`) and an iOS continuous-background-location keep-alive
(`ios_gateway_driver.dart`).

**Viewer role.** Displays a live aircraft list with planespotters.net photos,
route (origin → dest), and EMG/MIL badges. Emergency and military aircraft fire
**local push notifications** (`flutter_local_notifications`).

**BLE Wi-Fi provisioning.** A dedicated screen lets the user enter SSID +
password and sends an `encodeWifiConfig` packet to the wifi-config characteristic
`f1a90003-…`; the app then subscribes to NOTIFY and shows the result (connected
IP or failure reason) decoded by `parseWifiStatus`. Implemented in
`lib/ble/wifi_provisioner.dart` and `lib/packet/wifi_config_packet.dart`.

**Wi-Fi network scan (scan-to-pick).** A scan button next to the SSID field
triggers a `WifiScanner` BLE session (`lib/ble/wifi_scanner.dart`): it writes a
`"WS"` scan-request packet to `f1a90004-…`, collects the per-network NOTIFY
stream via a `ScanCollector` (`lib/packet/wifi_scan_packet.dart`), and on
completion opens a `NetworkPicker` bottom sheet (`lib/ui/network_picker.dart`)
listing each network with signal-strength icon, dBm value, and lock indicator.
Tapping a row fills the SSID field and focuses the password input. The scan,
feeder, and send operations are mutually exclusive (single central peripheral).
The device side of this flow is `src/wifi_scan_core.h` + the `f1a90004`
characteristic in `src/flight_ticker.ino`.

**Aircraft detail sheet.** Tapping any card on the home screen opens
`lib/ui/aircraft_detail_sheet.dart`: a live bottom sheet that subscribes to the
status stream filtered by hex. It shows a planespotters photo, EMG/MIL badges, a
full field grid (altitude, speed, track, squawk, route, distance, registration,
ICAO24, position, on-ground), and an OSM mini-map rendered via `flutter_map`
(aircraft marker rotated by track + observer dot). The sheet receives live
updates while open; if the aircraft drops out of the feed it shows a "Signal
lost" banner while retaining the last known data.

Stack: `flutter_blue_plus` (BLE), `geolocator` (GPS), `permission_handler`,
`flutter_local_notifications`, `flutter_map`, `latlong2`; the Dart packet
encoder in `lib/packet/` mirrors `src/ble_core.h` (v3), `src/wifi_config_core.h`,
and `src/wifi_scan_core.h`. `scripts/ble_send.py` remains the laptop smoke-test
harness.

## State machine

```
              tap
 RADAR ──────────────────────────────────►  DETAIL
   ▲    swipe up   = zoom in                   │
   │    swipe down = zoom out                  │ swipe-left  → next aircraft
   │    long-press = Wi-Fi portal              │ swipe-right → previous (wraps)
   │                                           │ (g_idx cycles through g_cache)
   │  tap / swipe-down / 15 s idle             │
   └───────────────────────────────────────── ─┘
```

- **RADAR**: rings + sweep + blips; nearest in-range aircraft highlighted with
  callsign; beyond-range aircraft as grey rim dots; observer dot at center;
  range readout top-center; "NO TRAFFIC" when the list is empty; red dot when
  stale. Swipe up/down zooms the display range; long-press reopens the captive
  portal.
- **DETAIL**: one aircraft (callsign, type + compass arrow + squawk, distance,
  altitude/speed, registration, operator airline code, route origin→dest) with
  page dots. Auto-returns to RADAR after 15 s of no touch, and bounces back
  immediately if the list becomes empty.

## Configuration

Tunables live in `src/config.h` (copied from `config.example.h`, gitignored):
`WIFI_SSID`/`WIFI_PASS` — seed credentials for a fresh device (WiFiManager
takes over after first connection); `MY_LAT`/`MY_LON` — compile-time default
observer location (overridden at runtime by NVS, written by the captive portal
or `saveLocation()`); `RADIUS_NM` — **legacy, unused at runtime** (the poll
radius is `queryRadiusNm(kRangePresets[kRangeCount-1])`); `POLL_INTERVAL_MS`;
`MAX_AIRCRAFT` (BLE decode cap + carousel length, currently 10); `IDLE_RETURN_MS`;
`SWEEP_PERIOD_MS`; `BLE_FRESHNESS_MS` (how long BLE-fed data counts as live
before falling back to `NO LINK`); `HIDE_GROUND_AIRCRAFT` (default `1`; drops
on-ground aircraft from the radar and list on both the Wi-Fi and BLE paths); and
the touch pins.

Runtime/NVS state: display range index (key `"rangeIdx"`, default 1 = 50 km) and
observer location (keys `"lat"`, `"lon"`), both in Preferences namespace
`"radar"`, are loaded at boot and persisted on change (zoom gesture or portal
save).

## Extending

- **More/fewer wire aircraft** → `MAX_AIRCRAFT` (BLE) and `RADAR_PLOT_CAP` (Wi-Fi parse cap).
- **Display range presets** → `kRangePresets` / `kRangeCount` in `render_core.h`.
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
