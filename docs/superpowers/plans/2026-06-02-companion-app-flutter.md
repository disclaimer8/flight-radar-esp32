# Flight Radar Companion App (Android v1) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a Flutter (Android) companion app that, in the background, reads the phone's GPS, fetches nearby aircraft from airplanes.live, and writes them as the device's binary BLE packet to the `FlightRadar` peripheral so the radar re-centers on the phone and plots live traffic.

**Architecture:** A pure, unit-tested core (Aircraft model, haversine, the BLE packet codec, the airplanes.live JSON parser) with no Flutter dependencies, plus a thin platform layer (flutter_blue_plus BLE, geolocator GPS, flutter_foreground_task background service) and a minimal status UI. The background loop runs inside the foreground-service isolate; UI state is pushed to the main isolate via `sendDataToMain`.

**Tech Stack:** Flutter/Dart, flutter_blue_plus, geolocator, flutter_foreground_task, http; flutter_test for the pure core.

**Prerequisites (do once, not a task):** Flutter SDK installed (`flutter doctor` clean for Android), an Android device with USB debugging or an emulator with BLE (a physical device is required for real BLE — emulators can't do BLE), and the flashed `FlightRadar` device powered nearby for the integration task.

---

## File Structure

All paths are under `companion/` (a new Flutter project in the mono-repo).

- `lib/data/aircraft.dart` — **new.** `Aircraft` model + `haversineKm` pure helper.
- `lib/packet/ble_packet.dart` — **new.** Pure wire-format codec: `encodePacket()` + constants. Mirrors `../src/ble_core.h`.
- `lib/data/airplanes_client.dart` — **new.** `parseAircraft()` (pure) + `AirplanesClient.fetchNearby()` (HTTP).
- `lib/ble/ble_manager.dart` — **new.** Scan/connect/write/reconnect over flutter_blue_plus.
- `lib/location/location_service.dart` — **new.** `LocationService` interface + `GeolocatorLocationService` impl.
- `lib/service/gateway_task_handler.dart` — **new.** The background `TaskHandler` loop + `startCallback`.
- `lib/service/gateway_controller.dart` — **new.** Main-isolate controller: init/start/stop service, expose status stream.
- `lib/ui/home_screen.dart` — **new.** Minimal status UI.
- `lib/main.dart` — **modify** (generated). App entry + permissions.
- `test/ble_packet_test.dart`, `test/aircraft_test.dart`, `test/airplanes_client_test.dart` — **new.** Unit tests.
- `android/app/src/main/AndroidManifest.xml` — **modify.** Permissions + service.
- `pubspec.yaml` — **modify.** Dependencies.

---

## Task 1: Scaffold the Flutter project + dependencies + manifest

**Files:**
- Create: the `companion/` Flutter project
- Modify: `companion/pubspec.yaml`, `companion/android/app/src/main/AndroidManifest.xml`

- [ ] **Step 1: Create the project (Android-only for v1)**

From the repo root (`flight-radar-esp32/`):

Run: `flutter create --org com.himaxym --project-name flight_radar_companion --platforms android companion`
Expected: a new `companion/` directory with a runnable Flutter app.

- [ ] **Step 2: Add dependencies**

Run (from `companion/`):
```bash
cd companion
flutter pub add flutter_blue_plus geolocator flutter_foreground_task http
```
Expected: `pubspec.yaml` gains the four packages; `flutter pub get` succeeds.

- [ ] **Step 3: Add Android permissions + service to the manifest**

In `android/app/src/main/AndroidManifest.xml`, add these **above** the `<application>` tag:

```xml
    <uses-permission android:name="android.permission.INTERNET"/>
    <!-- BLE (Android 12+) -->
    <uses-permission android:name="android.permission.BLUETOOTH_SCAN" android:usesPermissionFlags="neverForLocation"/>
    <uses-permission android:name="android.permission.BLUETOOTH_CONNECT"/>
    <!-- Location -->
    <uses-permission android:name="android.permission.ACCESS_FINE_LOCATION"/>
    <uses-permission android:name="android.permission.ACCESS_BACKGROUND_LOCATION"/>
    <!-- Foreground service -->
    <uses-permission android:name="android.permission.FOREGROUND_SERVICE"/>
    <uses-permission android:name="android.permission.FOREGROUND_SERVICE_LOCATION"/>
    <uses-permission android:name="android.permission.FOREGROUND_SERVICE_CONNECTED_DEVICE"/>
    <uses-permission android:name="android.permission.POST_NOTIFICATIONS"/>
    <uses-permission android:name="android.permission.WAKE_LOCK"/>
```

Inside `<application>`, add the flutter_foreground_task service (per its README), with the location + connectedDevice types:

```xml
        <service
            android:name="com.pravera.flutter_foreground_task.service.ForegroundService"
            android:foregroundServiceType="location|connectedDevice"
            android:exported="false" />
```

- [ ] **Step 4: Set minSdk**

flutter_blue_plus needs `minSdkVersion 21`+ and the BLE/location runtime permissions need 23+. In `android/app/build.gradle` (or `build.gradle.kts`), set `minSdk = 23` (or `minSdkVersion 23`).

- [ ] **Step 5: Verify it builds**

Run: `flutter analyze`
Expected: no errors (warnings about the default counter app are fine).
Run: `flutter build apk --debug`
Expected: `✓ Built build/app/outputs/flutter-apk/app-debug.apk`.

- [ ] **Step 6: Commit**

```bash
git add companion
git commit -m "feat(companion): scaffold Flutter Android project with deps + permissions"
```

---

## Task 2: Aircraft model + haversine helper (pure, TDD)

**Files:**
- Create: `companion/lib/data/aircraft.dart`
- Test: `companion/test/aircraft_test.dart`

- [ ] **Step 1: Write the failing test**

Create `companion/test/aircraft_test.dart`:

```dart
import 'package:flutter_test/flutter_test.dart';
import 'package:flight_radar_companion/data/aircraft.dart';

void main() {
  test('haversineKm matches a known city pair (London–Paris ~343 km)', () {
    final d = haversineKm(51.5074, -0.1278, 48.8566, 2.3522);
    expect(d, closeTo(343, 5));
  });

  test('haversineKm is zero for identical points', () {
    expect(haversineKm(38.0, -9.0, 38.0, -9.0), closeTo(0, 0.001));
  });

  test('Aircraft holds its fields', () {
    const a = Aircraft(
      callsign: 'RYR4KP', type: 'B738',
      lat: 38.8, lon: -9.28, altFt: 12000, gsKt: 420, onGround: false,
    );
    expect(a.callsign, 'RYR4KP');
    expect(a.altFt, 12000);
    expect(a.onGround, isFalse);
  });
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cd companion && flutter test test/aircraft_test.dart`
Expected: FAIL — `aircraft.dart` / `haversineKm` / `Aircraft` not found.

- [ ] **Step 3: Implement `lib/data/aircraft.dart`**

```dart
import 'dart:math' as math;

/// One aircraft. `altFt` / `gsKt` are null when the source value is missing
/// (they map to the wire packet's ALT_VALID / GS_VALID flags being clear).
class Aircraft {
  final String callsign;
  final String type;
  final double lat;
  final double lon;
  final int? altFt;
  final int? gsKt;
  final bool onGround;

  const Aircraft({
    required this.callsign,
    required this.type,
    required this.lat,
    required this.lon,
    required this.altFt,
    required this.gsKt,
    required this.onGround,
  });
}

/// Great-circle distance in kilometres. Mirror of `flight_core.h` haversineKm.
double haversineKm(double lat1, double lon1, double lat2, double lon2) {
  const r = 6371.0;
  final dLat = _rad(lat2 - lat1);
  final dLon = _rad(lon2 - lon1);
  final a = math.sin(dLat / 2) * math.sin(dLat / 2) +
      math.cos(_rad(lat1)) * math.cos(_rad(lat2)) *
          math.sin(dLon / 2) * math.sin(dLon / 2);
  return r * 2 * math.atan2(math.sqrt(a), math.sqrt(1 - a));
}

double _rad(double deg) => deg * math.pi / 180.0;
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `cd companion && flutter test test/aircraft_test.dart`
Expected: PASS (3 tests).

- [ ] **Step 5: Commit**

```bash
git add companion/lib/data/aircraft.dart companion/test/aircraft_test.dart
git commit -m "feat(companion): Aircraft model + haversine helper with tests"
```

---

## Task 3: BLE packet codec (pure, TDD) — the wire-format core

**Files:**
- Create: `companion/lib/packet/ble_packet.dart`
- Test: `companion/test/ble_packet_test.dart`

This must match `../src/ble_core.h` byte-for-byte (little-endian). Header 12 B, record 28 B, max 16 records.

- [ ] **Step 1: Write the failing test**

Create `companion/test/ble_packet_test.dart`:

```dart
import 'dart:typed_data';
import 'package:flutter_test/flutter_test.dart';
import 'package:flight_radar_companion/data/aircraft.dart';
import 'package:flight_radar_companion/packet/ble_packet.dart';

Aircraft _ac({
  String cs = 'AAA', String ty = 'A320',
  double lat = 0, double lon = 0, int? alt = 1000, int? gs = 300, bool ground = false,
}) => Aircraft(callsign: cs, type: ty, lat: lat, lon: lon, altFt: alt, gsKt: gs, onGround: ground);

void main() {
  test('empty packet is a 12-byte header', () {
    final bytes = encodePacket(48.0, 11.0, const []);
    expect(bytes.length, 12);
    expect(bytes[0], 0x46); // 'F'
    expect(bytes[1], 0x52); // 'R'
    expect(bytes[2], 1);    // version
    expect(bytes[3], 0);    // count
    final bd = ByteData.sublistView(bytes);
    expect(bd.getFloat32(4, Endian.little), closeTo(48.0, 0.0001));
    expect(bd.getFloat32(8, Endian.little), closeTo(11.0, 0.0001));
  });

  test('one record encodes fields at the correct offsets', () {
    final bytes = encodePacket(48.0, 11.0, [
      _ac(cs: 'RYR9XZ', ty: 'B738', lat: 48.1, lon: 11.2, alt: 12000, gs: 380, ground: false),
    ]);
    expect(bytes.length, 12 + 28);
    expect(bytes[3], 1); // count
    final r = ByteData.sublistView(bytes, 12);
    // callsign 8B ascii space-padded
    expect(String.fromCharCodes(bytes.sublist(12, 20)), 'RYR9XZ  ');
    // type 4B
    expect(String.fromCharCodes(bytes.sublist(20, 24)), 'B738');
    expect(r.getFloat32(12, Endian.little), closeTo(48.1, 0.0001)); // lat at record offset 12
    expect(r.getFloat32(16, Endian.little), closeTo(11.2, 0.0001)); // lon at 16
    expect(r.getInt32(20, Endian.little), 12000);                   // alt at 20
    expect(r.getInt16(24, Endian.little), 380);                     // gs at 24
    expect(r.getUint8(26), bleFlagAltValid | bleFlagGsValid);       // flags at 26
    expect(r.getUint8(27), 0);                                      // pad at 27
  });

  test('ground aircraft sets GROUND flag and clears alt/gs valid bits', () {
    final bytes = encodePacket(0, 0, [_ac(cs: 'GND1', alt: null, gs: null, ground: true)]);
    final flags = bytes[12 + 26];
    expect(flags & bleFlagGround, bleFlagGround);
    expect(flags & bleFlagAltValid, 0);
    expect(flags & bleFlagGsValid, 0);
  });

  test('long callsign/type are truncated to field width', () {
    final bytes = encodePacket(0, 0, [_ac(cs: 'TOOLONGCALL', ty: 'TYPED')]);
    expect(String.fromCharCodes(bytes.sublist(12, 20)), 'TOOLONGC'); // 8
    expect(String.fromCharCodes(bytes.sublist(20, 24)), 'TYPE');     // 4
  });

  test('count caps at 16 even if given more', () {
    final many = List.generate(20, (i) => _ac(cs: 'A$i'));
    final bytes = encodePacket(0, 0, many);
    expect(bytes[3], 16);
    expect(bytes.length, 12 + 16 * 28);
  });
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cd companion && flutter test test/ble_packet_test.dart`
Expected: FAIL — `ble_packet.dart` / `encodePacket` not found.

- [ ] **Step 3: Implement `lib/packet/ble_packet.dart`**

```dart
import 'dart:typed_data';
import '../data/aircraft.dart';

// Wire protocol — must match ../src/ble_core.h (little-endian).
const int bleMagic0 = 0x46; // 'F'
const int bleMagic1 = 0x52; // 'R'
const int bleVersion = 1;
const int bleMaxAircraft = 16;
const int bleHeaderSize = 12;
const int bleRecordSize = 28;

const int bleFlagGround = 0x01;
const int bleFlagAltValid = 0x02;
const int bleFlagGsValid = 0x04;

/// Encode one packet: 12-byte header + up to 16 × 28-byte records.
/// Records beyond 16 are dropped (the caller passes them nearest-first).
Uint8List encodePacket(double centerLat, double centerLon, List<Aircraft> aircraft) {
  final n = aircraft.length > bleMaxAircraft ? bleMaxAircraft : aircraft.length;
  final out = Uint8List(bleHeaderSize + n * bleRecordSize);
  final bd = ByteData.sublistView(out);

  out[0] = bleMagic0;
  out[1] = bleMagic1;
  out[2] = bleVersion;
  out[3] = n;
  bd.setFloat32(4, centerLat, Endian.little);
  bd.setFloat32(8, centerLon, Endian.little);

  for (var i = 0; i < n; i++) {
    final a = aircraft[i];
    final base = bleHeaderSize + i * bleRecordSize;
    _writeField(out, base, 8, a.callsign);
    _writeField(out, base + 8, 4, a.type);
    bd.setFloat32(base + 12, a.lat, Endian.little);
    bd.setFloat32(base + 16, a.lon, Endian.little);
    bd.setInt32(base + 20, a.altFt ?? 0, Endian.little);
    bd.setInt16(base + 24, a.gsKt ?? 0, Endian.little);
    var flags = 0;
    if (a.onGround) flags |= bleFlagGround;
    if (a.altFt != null) flags |= bleFlagAltValid;
    if (a.gsKt != null) flags |= bleFlagGsValid;
    out[base + 26] = flags;
    out[base + 27] = 0; // pad
  }
  return out;
}

/// Write an ASCII field of fixed width: truncate if longer, space-pad if shorter.
void _writeField(Uint8List out, int offset, int width, String s) {
  for (var i = 0; i < width; i++) {
    out[offset + i] = i < s.length ? (s.codeUnitAt(i) & 0x7f) : 0x20; // space
  }
}
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `cd companion && flutter test test/ble_packet_test.dart`
Expected: PASS (5 tests).

- [ ] **Step 5: Commit**

```bash
git add companion/lib/packet/ble_packet.dart companion/test/ble_packet_test.dart
git commit -m "feat(companion): BLE wire-packet encoder matching ble_core.h, with tests"
```

---

## Task 4: airplanes.live parser + fetch client (pure parse is TDD)

**Files:**
- Create: `companion/lib/data/airplanes_client.dart`
- Test: `companion/test/airplanes_client_test.dart`

Split the impure HTTP fetch from a pure `parseAircraft(jsonBody, centerLat, centerLon)` that maps + sorts nearest-first + trims to 16. Only the pure parser is unit-tested.

- [ ] **Step 1: Write the failing test**

Create `companion/test/airplanes_client_test.dart`:

```dart
import 'package:flutter_test/flutter_test.dart';
import 'package:flight_radar_companion/data/airplanes_client.dart';

// Minimal airplanes.live /v2/point response shape (only fields we read).
const _json = '''
{"ac":[
  {"flight":"RYR9XZ ","t":"B738","lat":48.50,"lon":11.00,"alt_baro":35000,"gs":453},
  {"flight":"DLH4AB ","t":"A320","lat":48.10,"lon":11.00,"alt_baro":12000,"gs":380},
  {"flight":"GRND01 ","t":"B772","lat":48.02,"lon":11.00,"alt_baro":"ground","gs":3},
  {"flight":"NOFIELD","t":"E190","lat":48.30,"lon":11.00}
]}
''';

void main() {
  test('parseAircraft maps fields, sorts nearest-first, and handles ground/missing', () {
    // center 48.0,11.0 — DLH(48.10) is nearest, RYR(48.50) farthest.
    final list = parseAircraft(_json, 48.0, 11.0);
    expect(list.length, 4);
    expect(list.first.callsign, 'GRND01'); // 48.02 is nearest
    expect(list.first.onGround, isTrue);
    expect(list.first.altFt, isNull);      // "ground" → alt invalid
    final ryr = list.firstWhere((a) => a.callsign == 'RYR9XZ');
    expect(ryr.type, 'B738');
    expect(ryr.altFt, 35000);
    expect(ryr.gsKt, 453);
    final noField = list.firstWhere((a) => a.callsign == 'NOFIELD');
    expect(noField.altFt, isNull);         // no alt_baro
    expect(noField.gsKt, isNull);          // no gs
  });

  test('parseAircraft caps to 16 nearest', () {
    final acs = List.generate(20, (i) =>
        '{"flight":"F$i","t":"A320","lat":${48.0 + i * 0.1},"lon":11.0,"alt_baro":1000,"gs":300}');
    final body = '{"ac":[${acs.join(",")}]}';
    final list = parseAircraft(body, 48.0, 11.0);
    expect(list.length, 16);
    expect(list.first.callsign, 'F0'); // nearest
  });

  test('parseAircraft tolerates empty / missing ac array', () {
    expect(parseAircraft('{"ac":[]}', 0, 0), isEmpty);
    expect(parseAircraft('{}', 0, 0), isEmpty);
  });
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cd companion && flutter test test/airplanes_client_test.dart`
Expected: FAIL — `airplanes_client.dart` / `parseAircraft` not found.

- [ ] **Step 3: Implement `lib/data/airplanes_client.dart`**

```dart
import 'dart:convert';
import 'package:http/http.dart' as http;
import 'aircraft.dart';
import '../packet/ble_packet.dart' show bleMaxAircraft;

/// Pure: map an airplanes.live /v2/point JSON body to aircraft, nearest-first,
/// capped to 16. Mirrors the field extraction in flight_core.h.
List<Aircraft> parseAircraft(String body, double centerLat, double centerLon) {
  final dynamic root = json.decode(body);
  final List<dynamic> ac = (root is Map && root['ac'] is List) ? root['ac'] as List : const [];

  final list = <Aircraft>[];
  for (final dynamic item in ac) {
    if (item is! Map) continue;
    final lat = _toDouble(item['lat']);
    final lon = _toDouble(item['lon']);
    if (lat == null || lon == null) continue;

    final altRaw = item['alt_baro'];
    final onGround = altRaw == 'ground';
    final int? altFt = (altRaw is num) ? altRaw.round() : null;
    final int? gsKt = (item['gs'] is num) ? (item['gs'] as num).round() : null;

    list.add(Aircraft(
      callsign: (item['flight'] as String?)?.trim() ?? '',
      type: (item['t'] as String?)?.trim() ?? '',
      lat: lat,
      lon: lon,
      altFt: altFt,
      gsKt: gsKt,
      onGround: onGround,
    ));
  }

  list.sort((a, b) => haversineKm(centerLat, centerLon, a.lat, a.lon)
      .compareTo(haversineKm(centerLat, centerLon, b.lat, b.lon)));
  if (list.length > bleMaxAircraft) list.removeRange(bleMaxAircraft, list.length);
  return list;
}

double? _toDouble(dynamic v) => (v is num) ? v.toDouble() : null;

/// Impure: fetch nearby aircraft from airplanes.live around (lat, lon).
class AirplanesClient {
  final http.Client _http;
  AirplanesClient([http.Client? client]) : _http = client ?? http.Client();

  /// Throws on a transport error or non-200 so the gateway can SKIP the cycle
  /// (vs. a 200 with an empty `ac` list, which legitimately means "no traffic").
  Future<List<Aircraft>> fetchNearby(double lat, double lon, int radiusNm) async {
    final uri = Uri.parse(
        'https://api.airplanes.live/v2/point/${lat.toStringAsFixed(4)}/${lon.toStringAsFixed(4)}/$radiusNm');
    final resp = await _http.get(uri, headers: {'User-Agent': 'flight-radar-companion'});
    if (resp.statusCode != 200) {
      throw Exception('airplanes.live HTTP ${resp.statusCode}');
    }
    return parseAircraft(resp.body, lat, lon);
  }
}
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `cd companion && flutter test test/airplanes_client_test.dart`
Expected: PASS (3 tests). Then run the whole suite: `flutter test` → all green.

- [ ] **Step 5: Commit**

```bash
git add companion/lib/data/airplanes_client.dart companion/test/airplanes_client_test.dart
git commit -m "feat(companion): airplanes.live parser (sort+cap) + fetch client, with tests"
```

---

## Task 5: BLE manager (flutter_blue_plus glue)

**Files:**
- Create: `companion/lib/ble/ble_manager.dart`

No host unit test (depends on the BLE platform); verified on-device in Task 9. Uses the flutter_blue_plus API: `startScan(withServices:[Guid(...)])`, `onScanResults`, `device.connect()`, `discoverServices()`, `characteristic.write(value, withoutResponse:false)`.

- [ ] **Step 1: Implement `lib/ble/ble_manager.dart`**

```dart
import 'dart:async';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';

enum BleStatus { idle, scanning, connecting, connected, disconnected }

/// Owns the BLE link to the FlightRadar device: scan → connect → write, with
/// reconnect. Designed to run inside the foreground-service isolate.
class BleManager {
  static final Guid serviceUuid = Guid('f1a90001-7e1d-4c2a-9b3f-1a2b3c4d5e6f');
  static final Guid charUuid = Guid('f1a90002-7e1d-4c2a-9b3f-1a2b3c4d5e6f');

  final _statusController = StreamController<BleStatus>.broadcast();
  Stream<BleStatus> get status => _statusController.stream;

  BluetoothDevice? _device;
  BluetoothCharacteristic? _char;
  StreamSubscription<List<ScanResult>>? _scanSub;
  StreamSubscription<BluetoothConnectionState>? _connSub;
  bool _wantConnected = false;

  BleStatus _status = BleStatus.idle;
  void _set(BleStatus s) { _status = s; _statusController.add(s); }
  BleStatus get current => _status;

  /// Begin scanning + connecting; keeps trying until [stop] is called.
  Future<void> start() async {
    _wantConnected = true;
    await _scanAndConnect();
  }

  Future<void> _scanAndConnect() async {
    if (!_wantConnected) return;
    _set(BleStatus.scanning);
    await FlutterBluePlus.adapterState.where((s) => s == BluetoothAdapterState.on).first;

    _scanSub?.cancel();
    _scanSub = FlutterBluePlus.onScanResults.listen((results) async {
      if (results.isEmpty) return;
      final r = results.first;
      await FlutterBluePlus.stopScan();
      await _scanSub?.cancel();
      await _connect(r.device);
    });

    await FlutterBluePlus.startScan(
      withServices: [serviceUuid],
      timeout: const Duration(seconds: 15),
    );
  }

  Future<void> _connect(BluetoothDevice device) async {
    _device = device;
    _set(BleStatus.connecting);
    _connSub?.cancel();
    _connSub = device.connectionState.listen((state) async {
      if (state == BluetoothConnectionState.disconnected) {
        _set(BleStatus.disconnected);
        _char = null;
        if (_wantConnected) await _scanAndConnect(); // reconnect
      }
    });
    try {
      await device.connect(timeout: const Duration(seconds: 15));
      final services = await device.discoverServices();
      for (final s in services) {
        if (s.uuid == serviceUuid) {
          for (final c in s.characteristics) {
            if (c.uuid == charUuid) _char = c;
          }
        }
      }
      _set(_char != null ? BleStatus.connected : BleStatus.disconnected);
    } catch (_) {
      _set(BleStatus.disconnected);
      if (_wantConnected) await _scanAndConnect();
    }
  }

  /// Write one packet. Returns true if a connected characteristic accepted it.
  Future<bool> sendPacket(List<int> bytes) async {
    final c = _char;
    if (c == null) return false;
    try {
      await c.write(bytes, withoutResponse: false); // write-with-response
      return true;
    } catch (_) {
      return false;
    }
  }

  Future<void> stop() async {
    _wantConnected = false;
    await _scanSub?.cancel();
    await _connSub?.cancel();
    try { await FlutterBluePlus.stopScan(); } catch (_) {}
    try { await _device?.disconnect(); } catch (_) {}
    _char = null;
    _set(BleStatus.idle);
  }

  void dispose() {
    _statusController.close();
  }
}
```

- [ ] **Step 2: Verify it analyzes**

Run: `cd companion && flutter analyze lib/ble/ble_manager.dart`
Expected: no errors. (Real behavior is checked in Task 9.)

- [ ] **Step 3: Commit**

```bash
git add companion/lib/ble/ble_manager.dart
git commit -m "feat(companion): BLE manager — scan/connect/write/reconnect to FlightRadar"
```

---

## Task 6: Location service (geolocator glue, behind an interface)

**Files:**
- Create: `companion/lib/location/location_service.dart`

Behind an interface so the iOS phase can swap the keep-alive implementation. No host test; verified on-device.

- [ ] **Step 1: Implement `lib/location/location_service.dart`**

```dart
import 'package:geolocator/geolocator.dart';

/// A 2D position the gateway centers the packet on.
class GpsFix {
  final double lat;
  final double lon;
  const GpsFix(this.lat, this.lon);
}

/// Abstracts location so the iOS phase can provide a different keep-alive impl.
abstract class LocationService {
  /// Ensure permission; returns false if unavailable/denied.
  Future<bool> ensurePermission();

  /// Latest fix, or null if none yet.
  Future<GpsFix?> currentFix();
}

class GeolocatorLocationService implements LocationService {
  @override
  Future<bool> ensurePermission() async {
    if (!await Geolocator.isLocationServiceEnabled()) return false;
    var p = await Geolocator.checkPermission();
    if (p == LocationPermission.denied) {
      p = await Geolocator.requestPermission();
    }
    return p == LocationPermission.always || p == LocationPermission.whileInUse;
  }

  @override
  Future<GpsFix?> currentFix() async {
    try {
      final pos = await Geolocator.getCurrentPosition(
        locationSettings: const LocationSettings(
          accuracy: LocationAccuracy.high,
          timeLimit: Duration(seconds: 10),
        ),
      );
      return GpsFix(pos.latitude, pos.longitude);
    } catch (_) {
      return null;
    }
  }
}
```

- [ ] **Step 2: Verify it analyzes**

Run: `cd companion && flutter analyze lib/location/location_service.dart`
Expected: no errors.

- [ ] **Step 3: Commit**

```bash
git add companion/lib/location/location_service.dart
git commit -m "feat(companion): LocationService interface + geolocator implementation"
```

---

## Task 7: Gateway TaskHandler + foreground service (the background loop)

**Files:**
- Create: `companion/lib/service/gateway_task_handler.dart`
- Create: `companion/lib/service/gateway_controller.dart`

The `TaskHandler` runs in the **foreground-service isolate**. It owns the BLE manager + location service, and on each repeat event does GPS → fetch → encode → write. It reports status to the UI isolate via `FlutterForegroundTask.sendDataToMain`.

- [ ] **Step 1: Implement the task handler `lib/service/gateway_task_handler.dart`**

```dart
import 'package:flutter_foreground_task/flutter_foreground_task.dart';
import '../ble/ble_manager.dart';
import '../data/airplanes_client.dart';
import '../location/location_service.dart';
import '../packet/ble_packet.dart';

const int kRadiusNm = 50;

@pragma('vm:entry-point')
void startCallback() {
  FlutterForegroundTask.setTaskHandler(GatewayTaskHandler());
}

/// Runs in the foreground-service isolate. One cycle per repeat event:
/// GPS fix → fetch airplanes.live → encode packet → BLE write.
class GatewayTaskHandler extends TaskHandler {
  final _ble = BleManager();
  final _location = GeolocatorLocationService();
  final _client = AirplanesClient();
  String _lastBle = 'idle';
  int _lastCount = 0;
  String _lastFix = 'no fix';

  @override
  Future<void> onStart(DateTime timestamp, TaskStarter starter) async {
    _ble.status.listen((s) {
      _lastBle = s.name;
      _push();
    });
    await _ble.start();
  }

  @override
  void onRepeatEvent(DateTime timestamp) {
    _cycle(); // fire-and-forget; the next repeat will run regardless
  }

  Future<void> _cycle() async {
    final fix = await _location.currentFix();
    if (fix == null) { _lastFix = 'no fix'; _push(); return; }
    _lastFix = '${fix.lat.toStringAsFixed(4)}, ${fix.lon.toStringAsFixed(4)}';

    try {
      final aircraft = await _client.fetchNearby(fix.lat, fix.lon, kRadiusNm);
      final packet = encodePacket(fix.lat, fix.lon, aircraft);
      final ok = await _ble.sendPacket(packet);
      if (ok) _lastCount = aircraft.length;
      FlutterForegroundTask.updateService(
        notificationTitle: 'Feeding Flight Radar',
        notificationText: ok ? 'Sent ${aircraft.length} aircraft' : 'Waiting for device…',
      );
    } catch (_) {
      // Offline / fetch failed: skip the send so we don't falsely refresh the
      // device's freshness window — let it fall back to NO LINK if truly offline.
      FlutterForegroundTask.updateService(
        notificationTitle: 'Feeding Flight Radar',
        notificationText: 'No data (offline?)',
      );
    }
    _push();
  }

  void _push() {
    FlutterForegroundTask.sendDataToMain({
      'ble': _lastBle,
      'count': _lastCount,
      'fix': _lastFix,
    });
  }

  @override
  Future<void> onDestroy(DateTime timestamp, bool isTimeout) async {
    await _ble.stop();
    _ble.dispose();
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

- [ ] **Step 2: Implement the main-isolate controller `lib/service/gateway_controller.dart`**

```dart
import 'dart:async';
import 'package:flutter/material.dart';
import 'package:flutter_foreground_task/flutter_foreground_task.dart';
import 'gateway_task_handler.dart';

/// Snapshot of gateway status shown in the UI.
class GatewayStatus {
  final String ble;
  final int count;
  final String fix;
  const GatewayStatus({this.ble = 'idle', this.count = 0, this.fix = 'no fix'});
}

/// Main-isolate side: initialize, start/stop the service, surface status.
class GatewayController {
  final _statusController = StreamController<GatewayStatus>.broadcast();
  Stream<GatewayStatus> get status => _statusController.stream;
  GatewayStatus _last = const GatewayStatus();

  void init() {
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
        eventAction: ForegroundTaskEventAction.repeat(10000), // 10 s
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
    await FlutterForegroundTask.stopService();
    _last = const GatewayStatus();
    _statusController.add(_last);
  }

  void dispose() {
    FlutterForegroundTask.removeTaskDataCallback(_onData);
    _statusController.close();
  }
}
```

- [ ] **Step 3: Verify it analyzes**

Run: `cd companion && flutter analyze lib/service`
Expected: no errors. (If an API name differs in the installed flutter_foreground_task version — e.g. `addTaskDataCallback` vs `addTaskDataCallback` — consult the installed package's README and adjust; the version resolved by `flutter pub add` is the source of truth.)

- [ ] **Step 4: Commit**

```bash
git add companion/lib/service
git commit -m "feat(companion): background gateway task handler + controller"
```

---

## Task 8: Home screen UI + main wire-up + permissions

**Files:**
- Create: `companion/lib/ui/home_screen.dart`
- Modify: `companion/lib/main.dart`

- [ ] **Step 1: Implement `lib/ui/home_screen.dart`**

```dart
import 'package:flutter/material.dart';
import 'package:flutter_foreground_task/flutter_foreground_task.dart';
import 'package:geolocator/geolocator.dart';
import '../service/gateway_controller.dart';

class HomeScreen extends StatefulWidget {
  const HomeScreen({super.key});
  @override
  State<HomeScreen> createState() => _HomeScreenState();
}

class _HomeScreenState extends State<HomeScreen> {
  final _controller = GatewayController();
  GatewayStatus _status = const GatewayStatus();
  bool _running = false;

  @override
  void initState() {
    super.initState();
    _controller.init();
    _controller.status.listen((s) => setState(() => _status = s));
  }

  Future<void> _requestPermissions() async {
    final perm = await FlutterForegroundTask.checkNotificationPermission();
    if (perm != NotificationPermission.granted) {
      await FlutterForegroundTask.requestNotificationPermission();
    }
    // Location (fine) then escalate to background ("Always") for the service.
    var p = await Geolocator.checkPermission();
    if (p == LocationPermission.denied) p = await Geolocator.requestPermission();
  }

  Future<void> _toggle() async {
    if (_running) {
      await _controller.stop();
      setState(() => _running = false);
    } else {
      await _requestPermissions();
      final ok = await _controller.start();
      setState(() => _running = ok);
    }
  }

  @override
  void dispose() {
    _controller.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('Flight Radar Companion')),
      body: Padding(
        padding: const EdgeInsets.all(24),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            _row('Device', _status.ble),
            _row('GPS', _status.fix),
            _row('Last packet', '${_status.count} aircraft'),
            const Spacer(),
            SizedBox(
              width: double.infinity,
              child: FilledButton(
                onPressed: _toggle,
                child: Text(_running ? 'Stop' : 'Start feeding device'),
              ),
            ),
          ],
        ),
      ),
    );
  }

  Widget _row(String label, String value) => Padding(
        padding: const EdgeInsets.symmetric(vertical: 8),
        child: Row(
          mainAxisAlignment: MainAxisAlignment.spaceBetween,
          children: [Text(label), Text(value, style: const TextStyle(fontWeight: FontWeight.bold))],
        ),
      );
}
```

- [ ] **Step 2: Replace `lib/main.dart`**

```dart
import 'package:flutter/material.dart';
import 'package:flutter_foreground_task/flutter_foreground_task.dart';
import 'ui/home_screen.dart';

void main() {
  WidgetsFlutterBinding.ensureInitialized();
  FlutterForegroundTask.initCommunicationPort();
  runApp(const CompanionApp());
}

class CompanionApp extends StatelessWidget {
  const CompanionApp({super.key});
  @override
  Widget build(BuildContext context) {
    // WithForegroundTask keeps the service alive across UI lifecycle.
    return MaterialApp(
      title: 'Flight Radar Companion',
      theme: ThemeData(colorSchemeSeed: Colors.indigo, useMaterial3: true),
      home: const WithForegroundTask(child: HomeScreen()),
    );
  }
}
```

- [ ] **Step 3: Verify it builds**

Run: `cd companion && flutter analyze`
Expected: no errors.
Run: `flutter build apk --debug`
Expected: build succeeds.

- [ ] **Step 4: Commit**

```bash
git add companion/lib/ui/home_screen.dart companion/lib/main.dart
git commit -m "feat(companion): minimal status UI + app entry + permission requests"
```

---

## Task 9: On-device integration verification

**Files:** none (device verification). Requires: an Android phone (USB-debugging on), the flashed `FlightRadar` device powered nearby with **Wi-Fi off** (so it would otherwise show NO LINK), and the phone with mobile data.

- [ ] **Step 1: Run on the phone**

Run: `cd companion && flutter run -d <android-device-id>` (`flutter devices` to list).
Expected: app installs and launches to the status screen.

- [ ] **Step 2: Grant permissions + start**

Tap **Start feeding device**. Grant notification, location (choose **Allow all the time** for background), and Bluetooth permissions when prompted.
Expected: a persistent notification appears ("Feeding Flight Radar"); within ~15 s the Device status moves `scanning → connecting → connected`; GPS shows coordinates; "Last packet" shows a non-zero aircraft count.

- [ ] **Step 3: Confirm on the device screen**

Expected: the radar's bottom indicator flips to cyan **B**, the radar re-centers on your location, and nearby real aircraft appear. Tapping the device opens the detail carousel over them.

- [ ] **Step 4: Confirm background operation**

Background the app (home button) and turn the phone screen off for ~1 minute.
Expected: the device keeps showing **B** (the notification stays, the loop keeps sending every 10 s). Bring the app back — status is still live.

- [ ] **Step 5: Confirm stop**

Tap **Stop** (or the notification's Stop button).
Expected: the notification clears; within `BLE_FRESHNESS_MS` (30 s) the device falls back to **NO LINK** (Wi-Fi still off).

- [ ] **Step 6: Note any issues.** Common ones and where to look:
  - BLE never connects → check `BLUETOOTH_SCAN`/`CONNECT` runtime grants and that the device is advertising (it always is); confirm `serviceUuid` matches.
  - Loop runs but device shows NO LINK → packet rejected; re-verify `encodePacket` against `src/ble_core.h` (Task 3 tests) and that `sendPacket` uses `withoutResponse:false`.
  - Service killed in background → ensure battery optimization is disabled for the app; confirm the foreground service types in the manifest include `location|connectedDevice`.
  Commit any fixes with a descriptive message.

---

## Done criteria (v1)

- `flutter test` (in `companion/`) passes — Aircraft/haversine, the byte-exact packet codec, and the airplanes.live parser.
- `flutter build apk --debug` succeeds.
- On a physical Android phone with the flashed device (Wi-Fi off): starting the app makes the device show cyan **B**, re-center on the phone's GPS, and plot live aircraft — and it keeps doing so with the app backgrounded and the screen off. Stopping returns the device to NO LINK.
