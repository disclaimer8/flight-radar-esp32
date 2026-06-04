# Flight Radar Companion

The phone companion app for the [Flight Radar](../README.md) ESP32 device — a
Flutter app for **Android and iOS** (hardware-verified). It does three things:

- **BLE feeder** (the original role) — polls
  [airplanes.live](https://airplanes.live) around the phone's own GPS, encodes the
  nearby aircraft as a binary packet (the device's **v3** wire format, mirrored in
  `lib/packet/ble_packet.dart`), and writes it to the device over BLE. This is the
  device's fallback data path **when it has no Wi-Fi**. It keeps running in the
  background via an Android foreground service and an iOS continuous-location
  keep-alive.
- **Live viewer** — the home screen is a scrollable list of the nearby aircraft,
  each card showing an aircraft **photo** (from planespotters.net), type,
  distance, route (origin→dest), registration, and **EMG/MIL** badges. It also
  fires a **local notification** when an emergency-squawk (7500/7600/7700) or
  military aircraft appears — even in the background.
- **Wi-Fi provisioning** — a "Configure device Wi-Fi" section: enter the device's
  network SSID + password and tap **Send to device**; the credentials travel over
  BLE (a dedicated wifi-config characteristic) and the device joins the network and
  reports status back. Requires the feeder to be **stopped** (the device accepts a
  single BLE central at a time).
- **Scan-to-pick network** — a scan button next to the SSID field requests the
  device to scan nearby networks over BLE (`f1a90004`); results stream back as
  per-network notifies and are presented in a picker sheet showing signal strength
  (dBm), lock icon, and SSID. Tapping a row fills the SSID and focuses the
  password field.
- **Aircraft detail sheet** — tap any card on the home screen to open a live
  bottom sheet: planespotters photo, EMG/MIL badges, full field grid
  (altitude/speed/track/squawk/route/distance/registration/ICAO24/position/on-ground),
  and an OSM mini-map (`flutter_map`) with a track-rotated aircraft marker and
  observer dot. Receives live updates from the status stream; shows a "Signal
  lost" banner if the aircraft drops out while the sheet is open.

## Architecture

- `lib/service/gateway_engine.dart` — the shared poll → enrich → encode → BLE-write
  cycle, plus emergency/military alert detection. Platform-agnostic.
- `lib/service/gateway_controller.dart` + `gateway_task_handler.dart` +
  `ios_gateway_driver.dart` — per-platform drivers (Android foreground service /
  iOS location keep-alive) and the status/aircraft-list bridge to the UI.
- `lib/data/` — `airplanes_client.dart` (poll + parse), `route_client.dart`
  (hexdb.io route lookup), `photo_client.dart` (planespotters photos),
  `aircraft.dart` (model + JSON).
- `lib/packet/ble_packet.dart` — v3 aircraft-packet encoder (byte-exact with
  `src/ble_core.h`). `lib/packet/wifi_config_packet.dart` — the Wi-Fi provisioning
  packet encoder + status decoder (byte-exact with `src/wifi_config_core.h`).
  `lib/packet/wifi_scan_packet.dart` — scan-request encoder + scan-record decoder +
  `ScanCollector` (byte-exact with `src/wifi_scan_core.h`).
- `lib/ble/ble_manager.dart` — the feeder's BLE link. `lib/ble/wifi_provisioner.dart`
  — the standalone on-demand provisioning flow. `lib/ble/device_finder.dart` — shared
  `findRadarDevice` helper (DRY between provisioner and scanner).
  `lib/ble/wifi_scanner.dart` — the scan session: writes request, collects notifies,
  returns the network list.
- `lib/ui/` — `home_screen.dart` (status + Start/Stop + provisioning section +
  aircraft list), `aircraft_card.dart`, `network_picker.dart` (bottom sheet for
  scan results), `aircraft_detail_sheet.dart` (live detail with OSM mini-map).

Key deps: `flutter_blue_plus` (BLE), `geolocator` (GPS), `flutter_foreground_task`
(Android background), `flutter_local_notifications` (alerts), `permission_handler`,
`http`, `flutter_map` (OSM mini-map in detail sheet), `latlong2` (coordinates).

## Run

```bash
flutter pub get
flutter test            # unit tests (packet parity, alerts, photo client, parsing)
flutter run --release   # on a connected device (iOS needs --release to run standalone)
```

On Android, `flutter_local_notifications` requires core-library desugaring — it's
already configured in `android/app/build.gradle.kts`.
