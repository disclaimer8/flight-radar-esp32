# Wi-Fi network picker (device-side scan) + aircraft detail sheet

Two companion-app improvements, one firmware protocol addition.

## Feature 1: Wi-Fi network picker

### Problem
Provisioning currently requires typing the SSID by hand. iOS offers no public
API to scan Wi-Fi networks from the phone, so the device must scan: the
networks that matter are the ones the **ESP32** can see anyway.

### BLE protocol (new characteristic `f1a90004`, WRITE + NOTIFY)
Separate characteristic, following the existing one-concern-per-characteristic
pattern (`0002` aircraft data, `0003` wifi-config). The `0003` provisioning
state machine is untouched.

**Scan request** (app → device, write):

| offset | size | field |
|---|---|---|
| 0 | 2 | magic `"WS"` (0x57 0x53) |
| 2 | 1 | version = 1 |

**Scan result** (device → app, one notify per network, ≤ 40 B — fits any MTU;
iOS may negotiate as low as 185):

| offset | size | field |
|---|---|---|
| 0 | 2 | magic `"WN"` (0x57 0x4E) |
| 2 | 1 | version = 1 |
| 3 | 1 | total networks (0 = none found / scan failed) |
| 4 | 1 | index (0-based) |
| 5 | 1 | rssi (int8, dBm) |
| 6 | 1 | secured (0 = open, 1 = secured) |
| 7 | 1 | ssidLen (1..32) |
| 8 | ≤32 | ssid (UTF-8, not NUL-terminated) |

A `total=0` notify is 4 bytes (magic + version + total only, no record
fields) and means "no networks found". Records are deduplicated by SSID
(strongest RSSI wins), sorted by RSSI descending, capped at 15, hidden
networks (empty SSID) dropped.

### Firmware
- `src/wifi_scan_core.h` — Arduino-free, host-tested: `buildScanRequest()` /
  `isScanRequest(buf, len)` / `encodeScanRecord(...)` / dedup-sort-cap helper
  operating on plain structs.
- `src/flight_ticker.ino`:
  - create `f1a90004` (WRITE | NOTIFY) next to the existing characteristics;
  - on valid scan-request write: set a flag (write callback only buffers/flags,
    `loop()` acts — same no-race rule as `0002`);
  - `loop()` starts `WiFi.scanNetworks(/*async=*/true)` (a blocking scan would
    freeze the radar for 2–3 s), then polls `WiFi.scanComplete()`; on
    completion encodes + notifies each record with a small delay (~20 ms)
    between notifies, then `WiFi.scanDelete()`;
  - scanning works in STA mode whether or not Wi-Fi is connected; BLE + Wi-Fi
    coexistence is already proven on this board;
  - ignore a new scan request while one is in flight.

### App
- `lib/packet/wifi_scan_packet.dart` — request builder + notify parser
  (mirror of `wifi_scan_core.h`, pure, unit-tested).
- `lib/ble/wifi_scanner.dart` — connect → subscribe to `0004` → write request
  → collect notifies until `count == total` or 15 s timeout → return
  `List<WifiNetwork>(ssid, rssi, secured)`. Reuses the connection approach of
  `WifiProvisioner`.
- UI (`home_screen.dart`): "Scan" button beside the SSID field → modal list
  (SSID, signal-strength icon by RSSI bucket, lock icon when secured) → tap
  fills the SSID field and focuses the password field. Errors (BLE off, device
  not found, timeout) surface in the existing `_provStatus` line.
- Scan is disabled while feeding (`_running`) or while provisioning — same
  rule as "Send to device".

## Feature 2: Aircraft detail sheet

Tap an `AircraftCard` → `showModalBottomSheet` (draggable, dismissible):

- header photo: existing `PhotoRef.thumbUrl` (already `thumbnail_large`) with
  photographer attribution + the existing flight-icon fallback;
- callsign + EMG/MIL badges, type description, registration;
- field grid: altitude (ft), ground speed (kt), track (°), squawk, route
  (origin → dest), distance (km), lat/lon, ICAO24 hex, on-ground;
  missing values render as "—";
- mini-map: `flutter_map` + OSM tiles (new deps `flutter_map`, `latlong2`; no
  API key; descriptive User-Agent per OSM tile policy), aircraft marker
  rotated by `track`, observer dot when the engine has a GPS fix;
- **live**: the sheet subscribes to `GatewayController.status`, re-finds its
  aircraft by `hex` each update; if the aircraft disappears from the feed the
  sheet stays open and shows a "signal lost" banner with the last known data.

No new network calls: everything is already in memory.

## Testing
- Host (`pio test -e native`): new `test_wifi_scan` cases — request
  round-trip, record encode/decode bounds (ssid 1/32/33, rssi sign, total=0),
  dedup/sort/cap helper.
- Flutter: `wifi_scan_packet` parser tests (incl. malformed/truncated),
  `wifi_scanner` collection logic with a fake stream, detail-sheet widget test
  (all fields render, "—" fallbacks, signal-lost banner), network-picker
  widget test (tap fills SSID). Existing 52 host + 40 Flutter tests stay
  green.

## Out of scope
- Phone-side Wi-Fi scanning (impossible on iOS without Apple-granted
  entitlements).
- Prefilling the phone's current SSID (can be added later; needs the
  "Access Wi-Fi Information" entitlement).
- External tracking links / browser handoff from the detail sheet.
