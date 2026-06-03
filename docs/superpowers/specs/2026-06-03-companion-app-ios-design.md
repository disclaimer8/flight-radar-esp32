# Flight Radar Companion — iOS Support (sub-project B, phase 2) — Design

> **Status:** approved design. Adds iOS to the existing Flutter companion app
> (`companion/`, Android v1 already shipped + hardware-verified). Full background
> parity via continuous-location keep-alive.

## Purpose

The companion app (phone → BLE gateway feeding the ESP32 radar) ships on Android.
This phase adds **iOS** with the same behavior: in the background, read phone GPS,
fetch nearby aircraft from airplanes.live, encode the device's binary BLE packet,
and write it to the `FlightRadar` peripheral so the radar re-centers on the phone.

## Key constraint that shapes the design

`flutter_foreground_task` on iOS uses BGTaskScheduler — background execution is
only ~30 seconds every ~15 minutes. That is useless for the ~10 s feed cadence.
So **iOS cannot use the Android foreground-service path.** Instead, iOS keeps the
app alive in the background with **continuous location updates** (the `location`
background mode + "Always" permission); the app's Dart event loop stays running,
so a `Timer.periodic(10 s)` drives the feed cycle. The location stream is the
keep-alive; the timer is the cadence.

## Decisions (from brainstorm)

- **Both platforms, but Android is already shipped/verified — do NOT rework it.**
  Chosen architecture = **shared `GatewayEngine` + per-platform drivers** (option A).
- **Full background parity on iOS** (continuous-location keep-alive), accepting the
  battery cost.
- **Free Apple ID** (personal team): dev signing, 7-day app expiry (re-install via
  `flutter run` weekly), no TestFlight, install directly to Denys's iPhone.
- **CocoaPods required.** `flutter_blue_plus` and `flutter_foreground_task` are
  podspec-only (no `Package.swift`), so Flutter's SPM falls back to CocoaPods for
  them regardless. Swapping both plugins to go CocoaPods-free is disproportionate
  (would rework the verified Android path). `brew install cocoapods` is one command.

## Architecture — refactor to a shared engine + thin drivers

### `lib/service/gateway_engine.dart` — **new, platform-agnostic**
Extract the feed-cycle logic out of the current `GatewayTaskHandler` into an engine
that owns the components and has no `flutter_foreground_task` dependency.

- Owns: `BleManager`, `GeolocatorLocationService` (as `LocationService`), `AirplanesClient`.
- `Stream<GatewayStatus> status` (broadcast) — emits `{ble, count, fix}`.
- `Future<void> start()` — `ble.start()`; subscribe to `ble.status` and re-emit into
  the engine's status stream (updating the `ble` field).
- `Future<void> runCycle()` — the cycle, with the `_busy` re-entrancy guard **here**
  (so both drivers are protected): `location.currentFix()` → on null, emit "no fix" and
  return; else `client.fetchNearby(fix, kRadiusNm=50)` (skip on throw), `encodePacket`,
  `ble.sendPacket`, update `count`/`fix`, emit status.
- `Future<void> stop()` — cancel the ble-status sub, `ble.stop()`, `ble.dispose()`.

`GatewayStatus` moves to (or is shared from) the engine file so both the engine and
the controller use one definition.

### `lib/service/gateway_task_handler.dart` — **Android driver, refactored to thin**
Runs in the foreground-service isolate (unchanged placement). Becomes a thin wrapper
over the engine so Android behavior is preserved 1:1:
- `onStart` → `engine.start()`; subscribe to `engine.status` → for each, `sendDataToMain`
  + `updateService` (notification text), exactly as today.
- `onRepeatEvent` → `engine.runCycle()` (the `_busy` guard now lives in the engine).
- `onDestroy` → `engine.stop()`.
- Notification button / pressed / dismissed callbacks unchanged.

### `lib/service/ios_gateway_driver.dart` — **new, iOS driver, main isolate**
- `Stream<GatewayStatus> get status` → proxies `engine.status`.
- `Future<bool> start()`:
  - Start the **keep-alive** background location stream:
    `Geolocator.getPositionStream(locationSettings: AppleSettings(accuracy: high,
    allowsBackgroundLocationUpdates: true, pauseLocationUpdatesAutomatically: false,
    showBackgroundLocationIndicator: true, activityType: other))` — subscribe and keep
    the subscription; its sole job is to keep the app alive in the background. The
    engine still gets each cycle's center via `location.currentFix()` (works while the
    app is kept alive), so the cycle code stays identical across platforms.
  - `engine.start()`.
  - `Timer.periodic(Duration(seconds: 10), (_) => engine.runCycle())`.
  - Return true.
- `Future<void> stop()`: cancel the timer, cancel the location-stream sub, `engine.stop()`.

### `lib/service/gateway_controller.dart` — **branch by platform**
- `init()`: Android → `initCommunicationPort()` + `FlutterForegroundTask.init(...)` +
  `addTaskDataCallback(_onData)` (as today). iOS → construct an `IosGatewayDriver`
  and subscribe to its `status` into the controller's status stream; no FGS init.
- `start()`: Android → `FlutterForegroundTask.startService(... callback: startCallback)`
  (as today). iOS → `_iosDriver.start()`.
- `stop()`: Android → `stopService()`. iOS → `_iosDriver.stop()`.
- `dispose()`: Android → `removeTaskDataCallback`; iOS → dispose the driver; both close
  the status controller.
- Platform check via `dart:io` `Platform.isIOS` / `Platform.isAndroid`.

The pure core (`ble_packet.dart`, `aircraft.dart`, `airplanes_client.dart`) and
`BleManager` / `LocationService` are unchanged — they already work cross-platform.

## iOS platform configuration

- `flutter create --platforms ios .` (run inside `companion/`) to generate the `ios/`
  folder (the project was created Android-only).
- Install CocoaPods: `brew install cocoapods`. (Flutter runs `pod install` on build.)
- `ios/Runner/Info.plist` keys:
  - `NSBluetoothAlwaysUsageDescription` — "Sends nearby aircraft to your Flight Radar device."
  - `NSLocationWhenInUseUsageDescription` — "Centers the radar on your location."
  - `NSLocationAlwaysAndWhenInUseUsageDescription` — "Keeps feeding the device in the background."
  - `UIBackgroundModes` = `[location, bluetooth-central]`.
- `permission_handler` iOS: in `ios/Podfile`, add the `GCC_PREPROCESSOR_DEFINITIONS`
  post-install macros enabling **only** `PERMISSION_LOCATION` and `PERMISSION_BLUETOOTH`
  (per permission_handler_apple setup; without this the pod compiles all permission
  stubs and App Store validation complains, and the requested ones may not work).
- Signing: open `ios/Runner.xcworkspace` in Xcode → Runner target → Signing & Capabilities
  → set bundle id `com.himaxym.flightRadarCompanion`, select the personal (free) team,
  enable automatic signing. Trust the developer profile on the iPhone
  (Settings → General → VPN & Device Management) on first run.

## Permissions flow (iOS)

iOS cannot request "Always" location directly — it must request "When in Use" first,
then escalate. `home_screen.dart`'s `_requestPermissions` branches by platform:
- **Android** (unchanged): `bluetoothScan`, `bluetoothConnect`, `locationWhenInUse`.
- **iOS**: `Permission.bluetooth` + `Permission.locationWhenInUse`, then (if granted)
  `Permission.locationAlways`. Return true if Bluetooth + at least when-in-use location
  are granted (background needs `always`, but the system upgrade prompt may be deferred —
  surface a hint if `always` is denied, since background won't work without it).

## Error handling

Same as Android (the engine is shared): BLE disconnect → auto-reconnect; no GPS fix →
skip the cycle; fetch failure/offline → skip the send (don't refresh device freshness);
permission denied → don't start, show a hint. iOS-specific: if "Always" location is
denied, background feeding stops when the app is backgrounded — surface that to the user.

## Testing

- The pure-core unit tests (15) are unchanged and cover iOS identically (same Dart).
- The `GatewayEngine` is glue (depends on concrete BleManager/Geolocator/Client);
  verified on-device, not unit-tested (consistent with the Android glue layers). No new
  host tests are required by this phase; `flutter test` must stay green (15).
- **Android regression check:** because the refactor touches the Android driver,
  `flutter analyze` + `flutter test` must pass, and ideally a quick on-device Android
  re-run (cyan B + background) to confirm the engine extraction preserved behavior.
- **iOS on-device** (the acceptance test), with the ESP32's Wi-Fi down and the iPhone on
  cellular/Wi-Fi for data: `flutter run -d <iphone>` → grant Bluetooth + location
  ("Always") → device shows cyan **B**, re-centers on the phone GPS, plots live aircraft;
  background the app + lock the screen → **B** holds and aircraft keep updating; Stop →
  device returns to **NO LINK**.

## File structure (delta)

- `lib/service/gateway_engine.dart` — **new** (shared cycle + status; `GatewayStatus`).
- `lib/service/ios_gateway_driver.dart` — **new** (iOS keep-alive + timer).
- `lib/service/gateway_task_handler.dart` — **modify** (delegate to engine).
- `lib/service/gateway_controller.dart` — **modify** (platform branch).
- `lib/ui/home_screen.dart` — **modify** (iOS permission branch).
- `ios/` — **new** (generated) + Info.plist + Podfile edits.
- `pubspec.yaml` — unchanged (same deps; iOS uses geolocator/flutter_blue_plus/
  permission_handler, not flutter_foreground_task).

## Out of scope

- TestFlight / App Store distribution (needs paid account).
- An iOS-native (non-Flutter) rewrite.
- Removing CocoaPods / swapping BLE or foreground-service plugins.
- macOS/other platforms.

## Done criteria

- `flutter analyze` clean; `flutter test` → 15 pass (Android path intact).
- On Denys's iPhone (free signing), with the ESP32 Wi-Fi down: the app makes the device
  show cyan **B**, re-center on the phone's GPS, and plot live aircraft — and keeps doing
  so with the app backgrounded and the screen locked. Stopping returns it to NO LINK.
- Android still works (engine refactor preserved behavior).
