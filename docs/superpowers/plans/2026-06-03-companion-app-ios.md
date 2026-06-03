# Companion App — iOS Support Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add iOS to the existing Flutter companion app (`companion/`, Android already shipped) with full background parity, by extracting a shared `GatewayEngine` and adding an iOS location-stream driver — without reworking the verified Android path.

**Architecture:** Pull the feed-cycle logic out of the Android `GatewayTaskHandler` into a platform-agnostic `GatewayEngine` (owns BLE/location/client, exposes a status stream). Android keeps its foreground-service driver (now thin); iOS gets a new driver that keeps the app alive with a continuous background-location stream and drives the cycle with a 10 s timer. `GatewayController` branches by platform.

**Tech Stack:** Flutter/Dart, flutter_blue_plus, geolocator (iOS background location), permission_handler, flutter_foreground_task (Android only), CocoaPods (iOS).

**Prerequisites:** `flutter`/`dart` on PATH. Tasks 1–4 are pure Dart (verified by `flutter analyze` + `flutter test` building for the host/Android — they compile even before the `ios/` folder exists, because geolocator re-exports `AppleSettings` from `package:geolocator/geolocator.dart`). Task 5 generates `ios/` and needs CocoaPods. Task 6 needs Denys's iPhone + Xcode (free personal team) + the ESP32 with Wi-Fi down.

---

## File Structure

All under `companion/`.

- `lib/service/gateway_engine.dart` — **new.** `GatewayStatus` + `GatewayEngine` (shared cycle, status stream, `_busy` guard). No flutter_foreground_task dependency.
- `lib/service/gateway_task_handler.dart` — **replace.** Thin Android driver delegating to the engine.
- `lib/service/ios_gateway_driver.dart` — **new.** iOS driver: background-location keep-alive + 10 s timer.
- `lib/service/gateway_controller.dart` — **replace.** Branches Android (FGS) vs iOS (driver).
- `lib/ui/home_screen.dart` — **modify.** iOS permission branch.
- `lib/main.dart` — **modify.** Guard `initCommunicationPort()` to Android.
- `ios/` — **new** (generated) + `Info.plist` + `Podfile` edits.

The pure core (`ble_packet.dart`, `aircraft.dart`, `airplanes_client.dart`) and `BleManager` / `LocationService` are unchanged.

---

## Task 1: Extract `GatewayEngine` and slim the Android handler

**Files:**
- Create: `companion/lib/service/gateway_engine.dart`
- Replace: `companion/lib/service/gateway_task_handler.dart`

No new host test (glue verified on-device). The existing 15 tests must stay green and `flutter analyze` clean — that is this task's gate. First READ both current files to understand the cycle you're extracting and to confirm method/field names.

- [ ] **Step 1: Create `companion/lib/service/gateway_engine.dart`**

```dart
import 'dart:async';
import '../ble/ble_manager.dart';
import '../data/airplanes_client.dart';
import '../location/location_service.dart';
import '../packet/ble_packet.dart';

const int kRadiusNm = 50;

/// Snapshot of gateway status for the UI.
class GatewayStatus {
  final String ble;
  final int count;
  final String fix;
  const GatewayStatus({this.ble = 'idle', this.count = 0, this.fix = 'no fix'});
}

/// Platform-agnostic feed engine: owns the BLE link, location, and the
/// airplanes.live client, and runs one cycle (GPS -> fetch -> encode -> BLE write)
/// per [runCycle] call. Drivers (Android foreground-service, iOS location-stream)
/// call start/runCycle/stop and forward [status]. No flutter_foreground_task dep.
class GatewayEngine {
  final BleManager _ble = BleManager();
  final LocationService _location = GeolocatorLocationService();
  final AirplanesClient _client = AirplanesClient();

  final _statusController = StreamController<GatewayStatus>.broadcast();
  Stream<GatewayStatus> get status => _statusController.stream;

  String _bleState = 'idle';
  int _count = 0;
  String _fix = 'no fix';
  StreamSubscription<BleStatus>? _bleSub;
  bool _busy = false;

  void _emit() {
    if (!_statusController.isClosed) {
      _statusController.add(GatewayStatus(ble: _bleState, count: _count, fix: _fix));
    }
  }

  /// Connect BLE and begin tracking its status. Call once before [runCycle].
  Future<void> start() async {
    _bleSub = _ble.status.listen((s) {
      _bleState = s.name;
      _emit();
    });
    await _ble.start();
  }

  /// One feed cycle. Re-entrancy-guarded: a tick is skipped if the previous
  /// cycle is still running (a cycle can exceed the driver's interval).
  Future<void> runCycle() async {
    if (_busy) return;
    _busy = true;
    try {
      await _cycle();
    } finally {
      _busy = false;
    }
  }

  Future<void> _cycle() async {
    final fix = await _location.currentFix();
    if (fix == null) {
      _fix = 'no fix';
      _emit();
      return;
    }
    _fix = '${fix.lat.toStringAsFixed(4)}, ${fix.lon.toStringAsFixed(4)}';
    try {
      final aircraft = await _client.fetchNearby(fix.lat, fix.lon, kRadiusNm);
      final packet = encodePacket(fix.lat, fix.lon, aircraft);
      final ok = await _ble.sendPacket(packet);
      if (ok) _count = aircraft.length;
    } catch (_) {
      // offline / fetch failed: skip the send so we don't refresh the device's
      // freshness window (let it fall back to NO LINK if truly offline).
    }
    _emit();
  }

  Future<void> stop() async {
    await _bleSub?.cancel();
    await _ble.stop();
    await _ble.dispose();
    _bleState = 'idle';
    _emit();
  }

  Future<void> dispose() async {
    await stop();
    await _statusController.close();
  }
}
```

- [ ] **Step 2: Replace `companion/lib/service/gateway_task_handler.dart` entirely**

```dart
import 'dart:async';
import 'package:flutter_foreground_task/flutter_foreground_task.dart';
import 'gateway_engine.dart';

@pragma('vm:entry-point')
void startCallback() {
  FlutterForegroundTask.setTaskHandler(GatewayTaskHandler());
}

/// Android driver: hosts the GatewayEngine in the foreground-service isolate,
/// forwards engine status to the UI isolate, and drives a cycle per repeat event.
class GatewayTaskHandler extends TaskHandler {
  final GatewayEngine _engine = GatewayEngine();
  StreamSubscription<GatewayStatus>? _sub;

  @override
  Future<void> onStart(DateTime timestamp, TaskStarter starter) async {
    _sub = _engine.status.listen((s) {
      FlutterForegroundTask.sendDataToMain({'ble': s.ble, 'count': s.count, 'fix': s.fix});
      final connected = s.ble == 'connected';
      FlutterForegroundTask.updateService(
        notificationTitle: 'Feeding Flight Radar',
        notificationText: connected ? 'Sent ${s.count} aircraft' : 'Waiting for device…',
      );
    });
    await _engine.start();
  }

  @override
  void onRepeatEvent(DateTime timestamp) {
    _engine.runCycle(); // the engine guards re-entrancy internally
  }

  @override
  Future<void> onDestroy(DateTime timestamp, bool isTimeout) async {
    await _sub?.cancel();
    await _engine.dispose();
  }

  @override
  void onReceiveData(Object data) {}
  @override
  void onNotificationButtonPressed(String id) {
    if (id == 'stop') FlutterForegroundTask.stopService();
  }
  @override
  void onNotificationPressed() => FlutterForegroundTask.launchApp('/');
  @override
  void onNotificationDismissed() {}
}
```

NOTE: `GatewayStatus` now lives in `gateway_engine.dart`. Task 3 removes the duplicate definition from `gateway_controller.dart`. Until Task 3, `gateway_controller.dart` still defines its own `GatewayStatus` and imports the handler — there will be a duplicate-symbol/analyze issue ONLY if both are imported in one library; they are not (controller doesn't import the handler's status). Verify analyze is clean after this task; if it flags a `GatewayStatus` conflict, proceed to Task 3 which resolves it. (`kRadiusNm` also moved to the engine; nothing else references it.)

- [ ] **Step 3: Verify**

Run: `cd companion && flutter analyze lib/service`
Expected: no errors.
Run: `cd companion && flutter test`
Expected: 15 pass (nothing else changed).

- [ ] **Step 4: Commit**

```bash
cd /Users/denyskolomiiets/flight-radar-esp32
git add companion/lib/service/gateway_engine.dart companion/lib/service/gateway_task_handler.dart
git commit -m "refactor(companion): extract platform-agnostic GatewayEngine; Android handler delegates"
```

---

## Task 2: iOS gateway driver

**Files:**
- Create: `companion/lib/service/ios_gateway_driver.dart`

No host test; verified on-device (Task 6). Uses geolocator's `AppleSettings` (re-exported from `package:geolocator/geolocator.dart`), so it compiles on the Android build too.

- [ ] **Step 1: Create `companion/lib/service/ios_gateway_driver.dart`**

```dart
import 'dart:async';
import 'package:geolocator/geolocator.dart';
import 'gateway_engine.dart';

/// iOS driver. iOS has no foreground service, so the app is kept alive in the
/// background by a continuous location stream (the `location` background mode +
/// "Always" permission). While alive, a periodic timer drives the feed cycle.
/// A fresh engine is created per [start] so the driver is restartable.
class IosGatewayDriver {
  final _statusController = StreamController<GatewayStatus>.broadcast();
  Stream<GatewayStatus> get status => _statusController.stream;

  GatewayEngine? _engine;
  StreamSubscription<GatewayStatus>? _engineSub;
  StreamSubscription<Position>? _keepAliveSub;
  Timer? _timer;

  Future<bool> start() async {
    final engine = GatewayEngine();
    _engine = engine;
    _engineSub = engine.status.listen((s) {
      if (!_statusController.isClosed) _statusController.add(s);
    });

    // Keep-alive: a continuous background location stream keeps the Dart event
    // loop running while backgrounded, so the timer keeps firing. The position
    // values are unused — the engine fetches its own fix per cycle.
    _keepAliveSub = Geolocator.getPositionStream(
      locationSettings: AppleSettings(
        accuracy: LocationAccuracy.high,
        allowsBackgroundLocationUpdates: true,
        pauseLocationUpdatesAutomatically: false,
        showBackgroundLocationIndicator: true,
        activityType: ActivityType.other,
      ),
    ).listen((_) {}, onError: (_) {});

    await engine.start();
    _timer = Timer.periodic(const Duration(seconds: 10), (_) => engine.runCycle());
    return true;
  }

  Future<void> stop() async {
    _timer?.cancel();
    _timer = null;
    await _keepAliveSub?.cancel();
    _keepAliveSub = null;
    await _engineSub?.cancel();
    _engineSub = null;
    await _engine?.dispose();
    _engine = null;
    if (!_statusController.isClosed) _statusController.add(const GatewayStatus());
  }

  Future<void> dispose() async {
    await stop();
    await _statusController.close();
  }
}
```

- [ ] **Step 2: Verify**

Run: `cd companion && flutter analyze lib/service/ios_gateway_driver.dart`
Expected: no errors. (If `AppleSettings`, `ActivityType`, or `allowsBackgroundLocationUpdates` aren't found, confirm the geolocator export path against the installed geolocator 14.0.2 source in `~/.pub-cache` and adjust the import; `AppleSettings` is re-exported from `package:geolocator/geolocator.dart`.)
Run: `cd companion && flutter test` → 15 pass.

- [ ] **Step 3: Commit**

```bash
cd /Users/denyskolomiiets/flight-radar-esp32
git add companion/lib/service/ios_gateway_driver.dart
git commit -m "feat(companion): iOS gateway driver — background-location keep-alive + 10s cycle"
```

---

## Task 3: Platform-branch the controller + main

**Files:**
- Replace: `companion/lib/service/gateway_controller.dart`
- Modify: `companion/lib/main.dart`

- [ ] **Step 1: Replace `companion/lib/service/gateway_controller.dart` entirely**

```dart
import 'dart:async';
import 'dart:io' show Platform;
import 'package:flutter_foreground_task/flutter_foreground_task.dart';
import 'gateway_engine.dart' show GatewayStatus;
import 'gateway_task_handler.dart';
import 'ios_gateway_driver.dart';

/// Main-isolate side: initialize, start/stop, and surface status. Branches by
/// platform — Android uses a foreground service; iOS runs the engine in-process
/// via [IosGatewayDriver].
class GatewayController {
  final _statusController = StreamController<GatewayStatus>.broadcast();
  Stream<GatewayStatus> get status => _statusController.stream;
  GatewayStatus _last = const GatewayStatus();

  IosGatewayDriver? _ios;
  StreamSubscription<GatewayStatus>? _iosSub;

  void init() {
    if (Platform.isIOS) {
      final ios = IosGatewayDriver();
      _ios = ios;
      _iosSub = ios.status.listen((s) {
        _last = s;
        _statusController.add(s);
      });
      return;
    }
    // Android: foreground service.
    FlutterForegroundTask.initCommunicationPort();
    FlutterForegroundTask.init(
      androidNotificationOptions: AndroidNotificationOptions(
        channelId: 'flight_radar_gateway',
        channelName: 'Flight Radar Gateway',
        channelDescription: 'Feeds aircraft to the device over BLE.',
        onlyAlertOnce: true,
      ),
      iosNotificationOptions: const IOSNotificationOptions(),
      foregroundTaskOptions: ForegroundTaskOptions(
        eventAction: ForegroundTaskEventAction.repeat(10000),
        autoRunOnBoot: false,
      ),
    );
    FlutterForegroundTask.addTaskDataCallback(_onData);
  }

  void _onData(Object data) {
    if (data is Map) {
      _last = GatewayStatus(
        ble: (data['ble'] as String?) ?? _last.ble,
        count: (data['count'] as int?) ?? _last.count,
        fix: (data['fix'] as String?) ?? _last.fix,
      );
      _statusController.add(_last);
    }
  }

  Future<bool> start() async {
    if (Platform.isIOS) {
      return await _ios?.start() ?? false;
    }
    final result = await FlutterForegroundTask.startService(
      serviceId: 256,
      notificationTitle: 'Feeding Flight Radar',
      notificationText: 'Starting…',
      notificationButtons: const [NotificationButton(id: 'stop', text: 'Stop')],
      callback: startCallback,
    );
    return result is ServiceRequestSuccess;
  }

  Future<void> stop() async {
    if (Platform.isIOS) {
      await _ios?.stop();
      return;
    }
    await FlutterForegroundTask.stopService();
    _last = const GatewayStatus();
    _statusController.add(_last);
  }

  void dispose() {
    if (Platform.isIOS) {
      _iosSub?.cancel();
      _ios?.dispose();
    } else {
      FlutterForegroundTask.removeTaskDataCallback(_onData);
    }
    _statusController.close();
  }
}
```

- [ ] **Step 2: Guard `initCommunicationPort` to Android in `companion/lib/main.dart`**

READ `companion/lib/main.dart`. Add `import 'dart:io' show Platform;` and wrap the `FlutterForegroundTask.initCommunicationPort();` call so it only runs on Android. The result should be:

```dart
import 'dart:io' show Platform;
import 'package:flutter/material.dart';
import 'package:flutter_foreground_task/flutter_foreground_task.dart';
import 'ui/home_screen.dart';

void main() {
  WidgetsFlutterBinding.ensureInitialized();
  if (Platform.isAndroid) {
    FlutterForegroundTask.initCommunicationPort();
  }
  runApp(const CompanionApp());
}

class CompanionApp extends StatelessWidget {
  const CompanionApp({super.key});
  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'Flight Radar Companion',
      theme: ThemeData(colorSchemeSeed: Colors.indigo, useMaterial3: true),
      home: const WithForegroundTask(child: HomeScreen()),
    );
  }
}
```
(`WithForegroundTask` is a cross-platform widget — it just renders its child on iOS — so it can stay.)

- [ ] **Step 3: Verify**

Run: `cd companion && flutter analyze`
Expected: no errors (the duplicate `GatewayStatus` is gone — it now lives only in the engine, and the controller imports it).
Run: `cd companion && flutter test`
Expected: 15 pass.

- [ ] **Step 4: Commit**

```bash
cd /Users/denyskolomiiets/flight-radar-esp32
git add companion/lib/service/gateway_controller.dart companion/lib/main.dart
git commit -m "feat(companion): platform-branch the gateway controller (Android FGS / iOS driver)"
```

---

## Task 4: iOS permission branch in the UI

**Files:**
- Modify: `companion/lib/ui/home_screen.dart`

- [ ] **Step 1: Add the platform import**

In `companion/lib/ui/home_screen.dart`, add at the top (after the existing imports):
```dart
import 'dart:io' show Platform;
```

- [ ] **Step 2: Replace the `_requestPermissions` method**

READ the current `_requestPermissions` (it currently requests notification + Android BT/location). Replace it with the platform-branched version:

```dart
  /// Request every runtime permission the gateway needs BEFORE start. On Android
  /// the location|connectedDevice FGS types require Bluetooth AND location granted
  /// at startForeground time. On iOS, background feeding needs "Always" location
  /// (requested as When-in-Use first, then escalated). Returns true if the
  /// required permissions are granted.
  Future<bool> _requestPermissions() async {
    if (Platform.isIOS) {
      final bt = await Permission.bluetooth.request();
      final whenInUse = await Permission.locationWhenInUse.request();
      if (whenInUse.isGranted) {
        // Background needs "Always"; iOS shows this as a follow-up upgrade prompt.
        await Permission.locationAlways.request();
      }
      return bt.isGranted && whenInUse.isGranted;
    }
    // Android.
    final notif = await FlutterForegroundTask.checkNotificationPermission();
    if (notif != NotificationPermission.granted) {
      await FlutterForegroundTask.requestNotificationPermission();
    }
    final statuses = await [
      Permission.bluetoothScan,
      Permission.bluetoothConnect,
      Permission.locationWhenInUse,
    ].request();
    return statuses.values.every((s) => s.isGranted);
  }
```

Leave `_toggle` (which calls `_requestPermissions` and shows the snackbar on denial) unchanged.

- [ ] **Step 3: Verify**

Run: `cd companion && flutter analyze`
Expected: no errors.
Run: `cd companion && flutter test`
Expected: 15 pass (the widget smoke test still renders).

- [ ] **Step 4: Commit**

```bash
cd /Users/denyskolomiiets/flight-radar-esp32
git add companion/lib/ui/home_screen.dart
git commit -m "feat(companion): iOS permission flow (Bluetooth + When-in-Use -> Always location)"
```

---

## Task 5: Generate the iOS platform + configure it

**Files:**
- Create: `companion/ios/` (generated)
- Modify: `companion/ios/Runner/Info.plist`, `companion/ios/Podfile`

This task needs CocoaPods. No device required — the gate is a successful no-codesign iOS build.

- [ ] **Step 1: Generate the iOS platform folder**

Run (inside `companion/`): `cd companion && flutter create --platforms ios --org com.himaxym .`
Expected: creates `companion/ios/` (and only ios — existing `lib/`, `android/`, `test/` untouched). Bundle id base becomes `com.himaxym.flightRadarCompanion`.

- [ ] **Step 2: Install CocoaPods**

Run: `brew install cocoapods`
Expected: `cocoapods` installed; `pod --version` prints a version.

- [ ] **Step 3: Add iOS usage descriptions + background modes to `companion/ios/Runner/Info.plist`**

Inside the top-level `<dict>` (e.g. before the closing `</dict></plist>`), add:

```xml
	<key>NSBluetoothAlwaysUsageDescription</key>
	<string>Sends nearby aircraft to your Flight Radar device over Bluetooth.</string>
	<key>NSLocationWhenInUseUsageDescription</key>
	<string>Centers the radar on your current location.</string>
	<key>NSLocationAlwaysAndWhenInUseUsageDescription</key>
	<string>Keeps feeding aircraft to your Flight Radar device while the app is in the background.</string>
	<key>UIBackgroundModes</key>
	<array>
		<string>location</string>
		<string>bluetooth-central</string>
	</array>
```

- [ ] **Step 4: Enable the permission_handler iOS macros in `companion/ios/Podfile`**

The generated Podfile has a `post_install do |installer| ... end` block that calls `flutter_additional_ios_build_settings(target)`. Edit that block so each target also gets the permission macros (enable ONLY the permissions we use). It should read:

```ruby
post_install do |installer|
  installer.pods_project.targets.each do |target|
    flutter_additional_ios_build_settings(target)
    target.build_configurations.each do |config|
      config.build_settings['GCC_PREPROCESSOR_DEFINITIONS'] ||= [
        '$(inherited)',
        'PERMISSION_LOCATION=1',
        'PERMISSION_BLUETOOTH=1',
      ]
    end
  end
end
```

Also ensure the Podfile's platform line is at least iOS 13 (geolocator/flutter_blue_plus need a modern min): set/uncomment `platform :ios, '13.0'` near the top.

- [ ] **Step 5: Install pods**

Run: `cd companion/ios && pod install`
Expected: `Pod installation complete!` (it resolves flutter_blue_plus, geolocator_apple, permission_handler_apple, flutter_foreground_task pods). If it complains the platform is too low, confirm Step 4's `platform :ios, '13.0'`.

- [ ] **Step 6: Build for iOS without signing (the verification gate)**

Run: `cd companion && flutter build ios --debug --no-codesign`
Expected: `✓ Built ...Runner.app`. This compiles all Dart (incl. the iOS driver) + the pods for a real-device target without needing a signing identity. If the Dart fails to compile for iOS, fix the offending code (most likely a geolocator API name) and re-run. If pods fail, re-check Steps 4–5.

- [ ] **Step 7: Commit**

```bash
cd /Users/denyskolomiiets/flight-radar-esp32
git add companion/ios
git commit -m "feat(companion): add iOS platform — Info.plist background modes + permission_handler pods"
```
NOTE: confirm `companion/.gitignore` / `companion/ios/.gitignore` exclude `Pods/`, `*.xcworkspace/xcuserdata`, `Flutter/Flutter.framework`, build output, and `.symlinks/` — `flutter create` generates an iOS `.gitignore` that does this. Verify `git status` shows no `Pods/` or build artifacts staged before committing.

---

## Task 6: iOS signing + on-device verification + Android regression

**Files:** none (device verification + Xcode signing). Needs: Denys's iPhone (USB or same-LAN wireless, Developer Mode on), the ESP32 powered with **Wi-Fi down** (advertising BLE as `FlightRadar`), and the iPhone on cellular/Wi-Fi for data.

- [ ] **Step 1: Set up free-account signing in Xcode**

Open: `open companion/ios/Runner.xcworkspace`
In Xcode → Runner target → Signing & Capabilities: check "Automatically manage signing", select Denys's personal team (free Apple ID — add it via Xcode → Settings → Accounts if absent), and confirm the bundle id is `com.himaxym.flightRadarCompanion` (change it if the team requires a unique id). Confirm a "Background Modes" capability shows Location updates + Uses Bluetooth LE accessories (these come from Info.plist).

- [ ] **Step 2: First run installs + trusts the app**

Run: `flutter devices` to get the iPhone's id, then `cd companion && flutter run -d <iphone-id>`.
On first launch iOS blocks the untrusted developer — on the iPhone: Settings → General → VPN & Device Management → trust the developer profile, then re-run.
Expected: the app launches to the status screen.

- [ ] **Step 3: Grant permissions and start**

Tap **Start feeding device**. Grant Bluetooth and Location. For location choose **Allow While Using**, then accept the follow-up **Change to Always Allow** prompt (required for background).
Expected: Device status `scanning → connecting → connected`; GPS shows coordinates; Last packet shows a non-zero count.

- [ ] **Step 4: Confirm on the device screen**

Expected: the radar's bottom indicator flips to cyan **B**, re-centers on your location, and plots nearby real aircraft.

- [ ] **Step 5: Confirm background operation**

Background the app and lock the iPhone for ~1 minute. iOS shows the blue location indicator (expected).
Expected: the device keeps showing **B** and aircraft keep updating (the location keep-alive holds the app alive; the 10 s timer keeps feeding).

- [ ] **Step 6: Confirm stop**

Foreground the app, tap **Stop**.
Expected: within `BLE_FRESHNESS_MS` (30 s) the device returns to **NO LINK** (Wi-Fi still down).

- [ ] **Step 7: Android regression check**

Confirm the engine refactor didn't break Android: `cd companion && flutter analyze && flutter test` (15 pass). If an Android phone is available, re-run the Android on-device flow (cyan B + background) to confirm parity. If no Android phone is handy, note that the analyze + tests + unchanged engine logic are the regression evidence.

- [ ] **Step 8: Note any issues.** Common iOS ones and where to look:
  - App suspends in background (B drops after ~30 s) → the location keep-alive isn't active: confirm **Always** location was granted, `UIBackgroundModes` has `location`, and `allowsBackgroundLocationUpdates: true` is set.
  - BLE never connects → confirm `bluetooth-central` background mode + `NSBluetoothAlwaysUsageDescription`; check the iPhone granted Bluetooth.
  - 7-day expiry: re-run `flutter run` to re-install when the dev cert lapses.
  Commit any fixes with a descriptive message.

---

## Done criteria

- `flutter analyze` clean; `flutter test` → 15 pass (Android path intact after the refactor).
- `flutter build ios --debug --no-codesign` succeeds.
- On Denys's iPhone (free signing) with the ESP32 Wi-Fi down: the app makes the device show cyan **B**, re-center on the phone's GPS, and plot live aircraft — and keeps doing so with the app backgrounded and the iPhone locked. Stopping returns the device to NO LINK.
- Android still works (engine refactor preserved behavior).
