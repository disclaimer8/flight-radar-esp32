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
- `lib/ble/ble_manager.dart` — the feeder's BLE link. `lib/ble/wifi_provisioner.dart`
  — the standalone on-demand provisioning flow.
- `lib/ui/` — `home_screen.dart` (status + Start/Stop + provisioning section +
  aircraft list) and `aircraft_card.dart`.

Key deps: `flutter_blue_plus` (BLE), `geolocator` (GPS), `flutter_foreground_task`
(Android background), `flutter_local_notifications` (alerts), `permission_handler`,
`http`.

## Run

```bash
flutter pub get
flutter test            # unit tests (packet parity, alerts, photo client, parsing)
flutter run --release   # on a connected device (iOS needs --release to run standalone)
```

On Android, `flutter_local_notifications` requires core-library desugaring — it's
already configured in `android/app/build.gradle.kts`.
