# Phone Enrichment (Photo/Route Viewer + Emergency/Military Push) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn the companion app's home screen into a live list of nearby aircraft with photos + routes, and fire a local notification when an emergency-squawk or military aircraft appears in a feed cycle.

**Architecture:** The feed engine already builds an enriched aircraft list per cycle; we surface that list to the UI (Android serializes it across the foreground-service isolate via `sendDataToMain`; iOS passes it in-process). The UI fetches a photo per card from planespotters.net. Emergency/military detection + notifications run inside the engine cycle so they work in the background. No firmware / BLE wire change — `hex`/`isMilitary`/`isEmergency` stay app-internal.

**Tech Stack:** Flutter (Dart), `http`, `flutter_local_notifications` (new), `flutter_foreground_task`, `flutter_test` + `package:http/testing.dart` (MockClient).

**Spec:** `docs/superpowers/specs/2026-06-03-phone-enrichment-design.md`

Work from `companion/`. Run tests with `flutter test`, lints with `flutter analyze`. `flutter` is on PATH (`~/development/flutter`, symlinked to `/opt/homebrew/bin`).

---

## File structure

- `lib/data/aircraft.dart` — `Aircraft` gains `hex`/`desc`/`isMilitary`/`isEmergency`/`distKm` + `toJson`/`fromJson`; `copyWith` gains `distKm`. **Task 1.**
- `lib/data/airplanes_client.dart` — `parseAircraft` extracts the new fields. **Task 2.**
- `lib/service/alerts.dart` (new) — pure alert predicate + de-dup + message text. **Task 3.**
- `lib/data/photo_client.dart` (new) — planespotters lookup + cache. **Task 4.**
- `pubspec.yaml` + `lib/service/notification_service.dart` (new) — notification dependency + wrapper. **Task 5.**
- `lib/service/gateway_engine.dart` — store the cycle's list (with `distKm`) on `GatewayStatus`; run alert detection + notify. **Task 6.**
- `lib/service/gateway_task_handler.dart` + `lib/service/gateway_controller.dart` — Android isolate serialize/deserialize of the list. **Task 7.**
- `lib/ui/aircraft_card.dart` (new) — one list card with photo. **Task 8.**
- `lib/ui/home_screen.dart` — home becomes the list. **Task 9.**
- Full verify + on-device. **Task 10.**

Pure Dart tasks (1-4) are TDD. Glue tasks (5-7, 9) verify by `flutter analyze` + `flutter test` (no regression). Widget task (8) has a widget test.

---

### Task 1: Aircraft model — new fields + JSON

**Files:** Modify `lib/data/aircraft.dart`; Test `test/aircraft_test.dart`.

- [ ] **Step 1: Write failing tests**

Append inside `main()` in `test/aircraft_test.dart`:

```dart
  test('Aircraft holds the new enrichment fields', () {
    const a = Aircraft(
      callsign: 'RRR2745', type: 'A400', lat: 51.0, lon: -1.0,
      altFt: 8000, gsKt: 300, onGround: false,
      hex: '43c123', desc: 'Airbus A400M', isMilitary: true,
      isEmergency: false, distKm: 12.5,
    );
    expect(a.hex, '43c123');
    expect(a.desc, 'Airbus A400M');
    expect(a.isMilitary, isTrue);
    expect(a.isEmergency, isFalse);
    expect(a.distKm, 12.5);
  });

  test('Aircraft toJson/fromJson round-trips all fields incl. nulls', () {
    const a = Aircraft(
      callsign: 'BAW117', type: 'A388', lat: 51.5, lon: -0.45,
      altFt: 35000, gsKt: 450, onGround: false, track: 287.0, squawk: 7700,
      registration: 'G-XLEA', origin: 'EGLL', dest: 'KJFK',
      hex: '40612a', desc: 'Airbus A380-841', isMilitary: false,
      isEmergency: true, distKm: 8.0,
    );
    final b = Aircraft.fromJson(a.toJson());
    expect(b.callsign, a.callsign);
    expect(b.lat, a.lat);
    expect(b.altFt, a.altFt);
    expect(b.track, a.track);
    expect(b.squawk, a.squawk);
    expect(b.registration, a.registration);
    expect(b.origin, a.origin);
    expect(b.dest, a.dest);
    expect(b.hex, a.hex);
    expect(b.desc, a.desc);
    expect(b.isEmergency, a.isEmergency);
    expect(b.distKm, a.distKm);

    // a missing-fields aircraft round-trips with safe defaults/nulls
    const c = Aircraft(callsign: 'X', type: '', lat: 0, lon: 0,
        altFt: null, gsKt: null, onGround: true);
    final d = Aircraft.fromJson(c.toJson());
    expect(d.altFt, isNull);
    expect(d.gsKt, isNull);
    expect(d.registration, isNull);
    expect(d.hex, '');
    expect(d.isMilitary, isFalse);
    expect(d.distKm, isNull);
  });
```

- [ ] **Step 2: Run, verify failure**

Run: `flutter test test/aircraft_test.dart`
Expected: FAIL — `hex`/`desc`/`isMilitary`/`isEmergency`/`distKm` not defined; `toJson`/`fromJson` missing.

- [ ] **Step 3: Implement**

In `lib/data/aircraft.dart`, replace the `Aircraft` class fields, constructor, and `copyWith` (lines ~5-38) with:

```dart
class Aircraft {
  final String callsign;
  final String type;
  final double lat;
  final double lon;
  final int? altFt;
  final int? gsKt;
  final bool onGround;
  final double? track; // true track degrees; null if missing
  final int? squawk;   // transponder code; null if missing
  final String? registration; // tail number; null if missing
  final String? origin; // origin ICAO; enriched later, null until then
  final String? dest;   // destination ICAO; enriched later, null until then
  final String hex;     // ICAO24 lowercase; "" if missing (photo fallback + alert id)
  final String desc;    // full type description; "" if missing (card subtitle)
  final bool isMilitary;  // dbFlags bit 0
  final bool isEmergency; // emergency squawk or emergency status field
  final double? distKm;   // distance from the GPS center; set by the engine

  const Aircraft({
    required this.callsign,
    required this.type,
    required this.lat,
    required this.lon,
    required this.altFt,
    required this.gsKt,
    required this.onGround,
    this.track,
    this.squawk,
    this.registration,
    this.origin,
    this.dest,
    this.hex = '',
    this.desc = '',
    this.isMilitary = false,
    this.isEmergency = false,
    this.distKm,
  });

  Aircraft copyWith({String? origin, String? dest, double? distKm}) => Aircraft(
        callsign: callsign, type: type, lat: lat, lon: lon, altFt: altFt,
        gsKt: gsKt, onGround: onGround, track: track, squawk: squawk,
        registration: registration, origin: origin ?? this.origin, dest: dest ?? this.dest,
        hex: hex, desc: desc, isMilitary: isMilitary, isEmergency: isEmergency,
        distKm: distKm ?? this.distKm,
      );

  Map<String, dynamic> toJson() => {
        'callsign': callsign, 'type': type, 'lat': lat, 'lon': lon,
        'altFt': altFt, 'gsKt': gsKt, 'onGround': onGround,
        'track': track, 'squawk': squawk, 'registration': registration,
        'origin': origin, 'dest': dest, 'hex': hex, 'desc': desc,
        'isMilitary': isMilitary, 'isEmergency': isEmergency, 'distKm': distKm,
      };

  factory Aircraft.fromJson(Map<String, dynamic> m) => Aircraft(
        callsign: m['callsign'] as String? ?? '',
        type: m['type'] as String? ?? '',
        lat: (m['lat'] as num?)?.toDouble() ?? 0,
        lon: (m['lon'] as num?)?.toDouble() ?? 0,
        altFt: (m['altFt'] as num?)?.toInt(),
        gsKt: (m['gsKt'] as num?)?.toInt(),
        onGround: m['onGround'] as bool? ?? false,
        track: (m['track'] as num?)?.toDouble(),
        squawk: (m['squawk'] as num?)?.toInt(),
        registration: m['registration'] as String?,
        origin: m['origin'] as String?,
        dest: m['dest'] as String?,
        hex: m['hex'] as String? ?? '',
        desc: m['desc'] as String? ?? '',
        isMilitary: m['isMilitary'] as bool? ?? false,
        isEmergency: m['isEmergency'] as bool? ?? false,
        distKm: (m['distKm'] as num?)?.toDouble(),
      );
}
```

- [ ] **Step 4: Run, verify pass**

Run: `flutter test test/aircraft_test.dart`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add lib/data/aircraft.dart test/aircraft_test.dart
git commit -m "feat(app): Aircraft hex/desc/military/emergency/distKm + JSON"
```

---

### Task 2: parseAircraft — extract new fields

**Files:** Modify `lib/data/airplanes_client.dart`; Test `test/airplanes_client_test.dart`.

- [ ] **Step 1: Write failing test**

Append inside `main()` in `test/airplanes_client_test.dart`:

```dart
  test('parseAircraft extracts hex, desc, military, emergency', () {
    const body = '''
{"ac":[
  {"flight":"RRR1","t":"A400","desc":"Airbus A400M","hex":"43C123","lat":0.0,"lon":0.1,"alt_baro":8000,"gs":300,"dbFlags":1},
  {"flight":"EMG1","t":"B738","hex":"AABBCC","lat":0.0,"lon":0.2,"alt_baro":10000,"gs":300,"squawk":"7700"},
  {"flight":"EMG2","t":"A320","hex":"DDEEFF","lat":0.0,"lon":0.3,"alt_baro":9000,"gs":300,"emergency":"general"},
  {"flight":"NORM","t":"A320","hex":"112233","lat":0.0,"lon":0.4,"alt_baro":9000,"gs":300,"squawk":"1200","emergency":"none","dbFlags":8}
]}
''';
    final list = parseAircraft(body, 0.0, 0.0);
    final rrr = list.firstWhere((a) => a.callsign == 'RRR1');
    expect(rrr.hex, '43c123');           // lowercased
    expect(rrr.desc, 'Airbus A400M');
    expect(rrr.isMilitary, isTrue);      // dbFlags & 1
    expect(rrr.isEmergency, isFalse);
    expect(list.firstWhere((a) => a.callsign == 'EMG1').isEmergency, isTrue);  // squawk 7700
    expect(list.firstWhere((a) => a.callsign == 'EMG2').isEmergency, isTrue);  // emergency field
    final norm = list.firstWhere((a) => a.callsign == 'NORM');
    expect(norm.isEmergency, isFalse);   // squawk 1200 + emergency "none"
    expect(norm.isMilitary, isFalse);    // dbFlags 8 (LADD), bit 0 clear
    expect(norm.hex, '112233');
  });
```

- [ ] **Step 2: Run, verify failure**

Run: `flutter test test/airplanes_client_test.dart`
Expected: FAIL — fields not populated (hex `''`, isMilitary/isEmergency false).

- [ ] **Step 3: Implement**

In `lib/data/airplanes_client.dart` `parseAircraft`, after the existing `registration` line (~line 32) add:

```dart
    final String hex = (item['hex'] as String?)?.toLowerCase() ?? '';
    final String desc = (item['desc'] as String?)?.trim() ?? '';
    final bool isMilitary = (((item['dbFlags'] as num?)?.toInt()) ?? 0) & 1 != 0;
    final em = item['emergency'] as String?;
    final bool emActive = em != null && em.isNotEmpty && em != 'none';
    final bool isEmergency =
        squawk == 7500 || squawk == 7600 || squawk == 7700 || emActive;
```

Then extend the `Aircraft(...)` constructor call (the `list.add(Aircraft(...))` block) to pass the new fields — add after `registration: registration,`:

```dart
      hex: hex,
      desc: desc,
      isMilitary: isMilitary,
      isEmergency: isEmergency,
```

- [ ] **Step 4: Run, verify pass**

Run: `flutter test test/airplanes_client_test.dart`
Expected: PASS (new test + all existing).

- [ ] **Step 5: Commit**

```bash
git add lib/data/airplanes_client.dart test/airplanes_client_test.dart
git commit -m "feat(app): parse hex/desc/military/emergency from airplanes.live"
```

---

### Task 3: alerts.dart — pure alert logic

**Files:** Create `lib/service/alerts.dart`; Test `test/alerts_test.dart`.

- [ ] **Step 1: Write failing test**

Create `test/alerts_test.dart`:

```dart
import 'package:flutter_test/flutter_test.dart';
import 'package:flight_radar_companion/data/aircraft.dart';
import 'package:flight_radar_companion/service/alerts.dart';

Aircraft _ac({String cs = 'AAA', String type = 'A320', String hex = 'abc123',
        bool mil = false, bool emg = false, int? squawk}) =>
    Aircraft(callsign: cs, type: type, lat: 0, lon: 0, altFt: 1000, gsKt: 200,
        onGround: false, squawk: squawk, hex: hex, isMilitary: mil, isEmergency: emg);

void main() {
  test('isAlertWorthy is true for emergency or military only', () {
    expect(isAlertWorthy(_ac(emg: true)), isTrue);
    expect(isAlertWorthy(_ac(mil: true)), isTrue);
    expect(isAlertWorthy(_ac()), isFalse);
  });

  test('computeNewAlerts flags first sighting then de-dups', () {
    final mil = _ac(cs: 'RRR1', hex: 'h1', mil: true);
    final r1 = computeNewAlerts([mil], <String>{});
    expect(r1.newAlerts.map((a) => a.hex), ['h1']);
    expect(r1.alerted, {'h1'});
    // same aircraft next cycle: no new alert, still tracked
    final r2 = computeNewAlerts([mil], r1.alerted);
    expect(r2.newAlerts, isEmpty);
    expect(r2.alerted, {'h1'});
  });

  test('computeNewAlerts re-alerts after the aircraft leaves and returns', () {
    final mil = _ac(hex: 'h1', mil: true);
    final gone = computeNewAlerts(const [], {'h1'}); // left
    expect(gone.alerted, isEmpty);
    final back = computeNewAlerts([mil], gone.alerted); // returned
    expect(back.newAlerts.map((a) => a.hex), ['h1']);
  });

  test('computeNewAlerts ignores non-worthy and empty-hex aircraft', () {
    final plain = _ac(hex: 'p1');
    final noHex = _ac(hex: '', mil: true);
    final r = computeNewAlerts([plain, noHex], <String>{});
    expect(r.newAlerts, isEmpty);
    expect(r.alerted, isEmpty);
  });

  test('alert text: emergency takes precedence, includes squawk', () {
    final e = _ac(cs: 'BAW117', emg: true, squawk: 7700);
    expect(alertTitle(e), 'Emergency squawk');
    expect(alertBody(e), contains('7700'));
    expect(alertBody(e), contains('BAW117'));
    final m = _ac(cs: 'RRR2745', type: 'A400', mil: true);
    expect(alertTitle(m), 'Military aircraft');
    expect(alertBody(m), contains('RRR2745'));
    expect(alertBody(m), contains('A400'));
  });
}
```

- [ ] **Step 2: Run, verify failure**

Run: `flutter test test/alerts_test.dart`
Expected: FAIL — `alerts.dart` does not exist.

- [ ] **Step 3: Implement**

Create `lib/service/alerts.dart`:

```dart
import '../data/aircraft.dart';

/// An aircraft worth alerting on: emergency squawk/status or military.
bool isAlertWorthy(Aircraft a) => a.isEmergency || a.isMilitary;

/// Notification title. Emergency takes precedence when an aircraft is both.
String alertTitle(Aircraft a) => a.isEmergency ? 'Emergency squawk' : 'Military aircraft';

/// Notification body.
String alertBody(Aircraft a) {
  final cs = a.callsign.isEmpty ? '------' : a.callsign;
  if (a.isEmergency) {
    final code = a.squawk?.toString() ?? '';
    return code.isEmpty ? '🚨 $cs' : '🚨 $code: $cs';
  }
  return '$cs ${a.type}'.trim();
}

/// Result of a de-dup pass: aircraft to alert now, and the set of qualifying
/// hexes seen in this cycle (the next cycle's "previously alerted").
typedef AlertPass = ({List<Aircraft> newAlerts, Set<String> alerted});

/// Among [current], pick alert-worthy aircraft (with a non-empty hex) whose hex
/// was not in [previouslyAlerted]. The returned [alerted] set is exactly the
/// qualifying hexes present this cycle — so an aircraft that leaves and returns
/// alerts again, while steady-state presence does not re-alert.
AlertPass computeNewAlerts(List<Aircraft> current, Set<String> previouslyAlerted) {
  final qualifying = current.where((a) => isAlertWorthy(a) && a.hex.isNotEmpty).toList();
  final alerted = qualifying.map((a) => a.hex).toSet();
  final newAlerts = qualifying.where((a) => !previouslyAlerted.contains(a.hex)).toList();
  return (newAlerts: newAlerts, alerted: alerted);
}
```

- [ ] **Step 4: Run, verify pass**

Run: `flutter test test/alerts_test.dart`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add lib/service/alerts.dart test/alerts_test.dart
git commit -m "feat(app): pure emergency/military alert predicate + de-dup"
```

---

### Task 4: photo_client.dart — planespotters lookup + cache

**Files:** Create `lib/data/photo_client.dart`; Test `test/photo_client_test.dart`.

- [ ] **Step 1: Write failing test**

Create `test/photo_client_test.dart`:

```dart
import 'package:flutter_test/flutter_test.dart';
import 'package:http/http.dart' as http;
import 'package:http/testing.dart';
import 'package:flight_radar_companion/data/photo_client.dart';

String _photoJson(String src) => '{"photos":[{"thumbnail_large":{"src":"$src","size":{"width":419,"height":280}},'
    '"link":"https://planespotters.net/photo/1","photographer":"Jane Doe"}]}';
const _empty = '{"photos":[]}';

void main() {
  test('lookup hits by registration', () async {
    var calls = 0;
    final c = PhotoClient(MockClient((req) async {
      calls++;
      expect(req.headers['User-Agent'], contains('flight-radar-esp32-companion'));
      expect(req.url.path, '/pub/photos/reg/D-AIMA');
      return http.Response(_photoJson('https://t/x.jpg'), 200);
    }));
    final p = await c.lookup(reg: 'D-AIMA', hex: '3c4ad2');
    expect(p, isNotNull);
    expect(p!.thumbUrl, 'https://t/x.jpg');
    expect(p.photographer, 'Jane Doe');
    expect(calls, 1); // reg hit, no hex call
  });

  test('lookup falls back to hex when reg has no photo', () async {
    final c = PhotoClient(MockClient((req) async {
      if (req.url.path.contains('/reg/')) return http.Response(_empty, 200);
      return http.Response(_photoJson('https://t/h.jpg'), 200);
    }));
    final p = await c.lookup(reg: 'NOREG', hex: '3c4ad2');
    expect(p, isNotNull);
    expect(p!.thumbUrl, 'https://t/h.jpg');
  });

  test('both miss -> null, cached (no second HTTP call)', () async {
    var calls = 0;
    final c = PhotoClient(MockClient((req) async {
      calls++;
      return http.Response(_empty, 200);
    }));
    expect(await c.lookup(reg: 'NOPE', hex: 'beef'), isNull);
    final before = calls;
    expect(await c.lookup(reg: 'NOPE', hex: 'beef'), isNull);
    expect(calls, before); // served from cache
  });

  test('non-200 -> null', () async {
    final c = PhotoClient(MockClient((req) async => http.Response('nope', 403)));
    expect(await c.lookup(reg: 'X', hex: ''), isNull);
  });
}
```

- [ ] **Step 2: Run, verify failure**

Run: `flutter test test/photo_client_test.dart`
Expected: FAIL — `photo_client.dart` missing.

- [ ] **Step 3: Implement**

Create `lib/data/photo_client.dart`:

```dart
import 'dart:convert';
import 'package:http/http.dart' as http;

/// A single aircraft photo reference from planespotters.net.
class PhotoRef {
  final String thumbUrl;
  final String photographer;
  final String link;
  const PhotoRef(this.thumbUrl, this.photographer, this.link);
}

/// Fetches aircraft photos from the planespotters.net public API, by registration
/// then hex, caching results (including misses) by lookup key. Photos are fetched
/// only from the UI (foreground); never from the background feed cycle.
class PhotoClient {
  // planespotters rejects generic User-Agents (HTTP 403) — must be descriptive
  // with a contact URL.
  static const _ua =
      'flight-radar-esp32-companion/1.0 (+https://github.com/disclaimer8/flight-radar-esp32)';
  final http.Client _http;
  final Map<String, PhotoRef?> _cache = {};
  PhotoClient([http.Client? client]) : _http = client ?? http.Client();

  /// Try [reg] first, then [hex]; the first hit wins, else null.
  Future<PhotoRef?> lookup({required String reg, required String hex}) async {
    if (reg.isNotEmpty) {
      final r = await _byKey('reg', reg);
      if (r != null) return r;
    }
    if (hex.isNotEmpty) {
      final r = await _byKey('hex', hex);
      if (r != null) return r;
    }
    return null;
  }

  Future<PhotoRef?> _byKey(String kind, String value) async {
    final cacheKey = '$kind/$value';
    if (_cache.containsKey(cacheKey)) return _cache[cacheKey];
    PhotoRef? result;
    try {
      final resp = await _http.get(
        Uri.parse('https://api.planespotters.net/pub/photos/$kind/$value'),
        headers: {'User-Agent': _ua},
      ).timeout(const Duration(seconds: 8));
      if (resp.statusCode == 200) {
        final m = json.decode(resp.body);
        if (m is Map && m['photos'] is List && (m['photos'] as List).isNotEmpty) {
          final p = (m['photos'] as List).first as Map;
          final thumb = (p['thumbnail_large'] ?? p['thumbnail']) as Map?;
          final src = thumb?['src'] as String?;
          if (src != null) {
            result = PhotoRef(src, (p['photographer'] as String?) ?? '',
                (p['link'] as String?) ?? '');
          }
        }
      }
    } catch (_) {/* leave as a miss */}
    _cache[cacheKey] = result;
    return result;
  }
}
```

- [ ] **Step 4: Run, verify pass**

Run: `flutter test test/photo_client_test.dart`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add lib/data/photo_client.dart test/photo_client_test.dart
git commit -m "feat(app): planespotters PhotoClient (reg->hex, cached, descriptive UA)"
```

---

### Task 5: notification dependency + NotificationService

**Files:** Modify `pubspec.yaml`; Create `lib/service/notification_service.dart`.

Glue (platform plugin); verify by `flutter analyze` + `flutter test` (compiles, no regression).

- [ ] **Step 1: Add the dependency**

Run: `flutter pub add flutter_local_notifications`
Expected: resolves a compatible version, updates `pubspec.yaml` + `pubspec.lock`, `flutter pub get` succeeds.

- [ ] **Step 2: Implement the wrapper**

Create `lib/service/notification_service.dart`:

```dart
import 'package:flutter_local_notifications/flutter_local_notifications.dart';

/// Thin wrapper over flutter_local_notifications for emergency/military alerts.
/// init() must run in the isolate that will call show() — the engine inits it in
/// start() (Android foreground-service isolate / iOS main isolate).
class NotificationService {
  final FlutterLocalNotificationsPlugin _plugin = FlutterLocalNotificationsPlugin();
  bool _ready = false;

  Future<void> init() async {
    const android = AndroidInitializationSettings('@mipmap/ic_launcher');
    const darwin = DarwinInitializationSettings(
      requestAlertPermission: true,
      requestBadgePermission: false,
      requestSoundPermission: true,
    );
    await _plugin.initialize(
        const InitializationSettings(android: android, iOS: darwin));
    await _plugin
        .resolvePlatformSpecificImplementation<
            AndroidFlutterLocalNotificationsPlugin>()
        ?.requestNotificationsPermission();
    _ready = true;
  }

  Future<void> show(int id, String title, String body) async {
    if (!_ready) return;
    const details = NotificationDetails(
      android: AndroidNotificationDetails(
        'flight_radar_alerts', 'Flight Radar Alerts',
        channelDescription: 'Emergency and military aircraft alerts',
        importance: Importance.high, priority: Priority.high,
      ),
      iOS: DarwinNotificationDetails(),
    );
    await _plugin.show(id, title, body, details);
  }
}
```

- [ ] **Step 3: Verify**

Run: `flutter analyze` → no new issues. Then `flutter test` → all existing pass (the import compiles).

- [ ] **Step 4: Commit**

```bash
git add pubspec.yaml pubspec.lock lib/service/notification_service.dart
git commit -m "feat(app): add flutter_local_notifications + NotificationService wrapper"
```

---

### Task 6: gateway_engine — list-to-status + alert detection

**Files:** Modify `lib/service/gateway_engine.dart`.

Glue; verify by `flutter analyze` + `flutter test`.

- [ ] **Step 1: Add imports + state**

In `lib/service/gateway_engine.dart`, add to the imports:

```dart
import 'alerts.dart';
import 'notification_service.dart';
```

Add fields to `GatewayEngine` (near the other private fields, after `bool _busy = false;`):

```dart
  final NotificationService _notify = NotificationService();
  List<Aircraft> _lastAircraft = const [];
  Set<String> _alerted = {};
```

- [ ] **Step 2: Carry the list on GatewayStatus**

Replace the `GatewayStatus` class (top of file) with:

```dart
/// Snapshot of gateway status for the UI.
class GatewayStatus {
  final String ble;
  final int count;
  final String fix;
  final List<Aircraft> aircraft;
  const GatewayStatus(
      {this.ble = 'idle', this.count = 0, this.fix = 'no fix', this.aircraft = const []});
}
```

Add `import '../data/aircraft.dart';` if not already present (it is). Update `_emit()`:

```dart
  void _emit() {
    if (!_statusController.isClosed) {
      _statusController.add(GatewayStatus(
          ble: _bleState, count: _count, fix: _fix, aircraft: _lastAircraft));
    }
  }
```

- [ ] **Step 3: Init notifications in start()**

In `start()`, add before `await _ble.start();`:

```dart
    await _notify.init();
```

- [ ] **Step 4: Compute distKm, store the list, detect alerts in _cycle**

Replace the body of the `try` block in `_cycle` (the `final aircraft = ...` through `if (ok) _count = ...`) with:

```dart
      final aircraft = await _client.fetchNearby(fix.lat, fix.lon, kRadiusNm);
      final enriched = <Aircraft>[];
      for (final a in aircraft) {
        final (o, d) = await _routes.lookup(a.callsign);
        final dist = haversineKm(fix.lat, fix.lon, a.lat, a.lon);
        var e = a.copyWith(distKm: dist);
        if (o.isNotEmpty) e = e.copyWith(origin: o, dest: d);
        enriched.add(e);
      }
      _lastAircraft = enriched;

      // Emergency/military alerts (run regardless of foreground/background). A
      // notification failure must never abort the feed.
      final pass = computeNewAlerts(enriched, _alerted);
      _alerted = pass.alerted;
      for (final a in pass.newAlerts) {
        try {
          await _notify.show(a.hex.hashCode & 0x7fffffff, alertTitle(a), alertBody(a));
        } catch (_) {/* ignore */}
      }

      final packet = encodePacket(fix.lat, fix.lon, enriched);
      final ok = await _ble.sendPacket(packet);
      if (ok) _count = aircraft.length;
```

(`haversineKm` is already exported from `aircraft.dart`, which the engine imports.)

- [ ] **Step 5: Verify**

Run: `flutter analyze` → clean. Then `flutter test` → all pass (no regression; `computeNewAlerts` already covered by Task 3).

- [ ] **Step 6: Commit**

```bash
git add lib/service/gateway_engine.dart
git commit -m "feat(app): engine surfaces aircraft list + fires emergency/military alerts"
```

---

### Task 7: Android isolate plumbing (serialize/deserialize the list)

**Files:** Modify `lib/service/gateway_task_handler.dart`, `lib/service/gateway_controller.dart`.

Glue; verify by `flutter analyze` + `flutter test`.

- [ ] **Step 1: Serialize the list to the main isolate**

In `lib/service/gateway_task_handler.dart` `onStart`, replace the `sendDataToMain` call with:

```dart
      FlutterForegroundTask.sendDataToMain({
        'ble': s.ble,
        'count': s.count,
        'fix': s.fix,
        'aircraft': s.aircraft.map((a) => a.toJson()).toList(),
      });
```

- [ ] **Step 2: Deserialize in the controller**

In `lib/service/gateway_controller.dart`, add the import:

```dart
import '../data/aircraft.dart';
```

Replace `_onData` with:

```dart
  void _onData(Object data) {
    if (data is Map) {
      List<Aircraft> aircraft = _last.aircraft;
      if (data['aircraft'] is List) {
        aircraft = (data['aircraft'] as List)
            .whereType<Map>()
            .map((m) => Aircraft.fromJson(Map<String, dynamic>.from(m)))
            .toList();
      }
      _last = GatewayStatus(
        ble: (data['ble'] as String?) ?? _last.ble,
        count: (data['count'] as int?) ?? _last.count,
        fix: (data['fix'] as String?) ?? _last.fix,
        aircraft: aircraft,
      );
      _statusController.add(_last);
    }
  }
```

- [ ] **Step 3: Verify**

Run: `flutter analyze` → clean. Then `flutter test` → all pass.

- [ ] **Step 4: Commit**

```bash
git add lib/service/gateway_task_handler.dart lib/service/gateway_controller.dart
git commit -m "feat(app): serialize aircraft list across the Android FGS isolate"
```

---

### Task 8: aircraft_card.dart — list card with photo

**Files:** Create `lib/ui/aircraft_card.dart`; Test `test/aircraft_card_test.dart`.

- [ ] **Step 1: Write failing widget test**

Create `test/aircraft_card_test.dart`:

```dart
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:http/testing.dart';
import 'package:http/http.dart' as http;
import 'package:flight_radar_companion/data/aircraft.dart';
import 'package:flight_radar_companion/data/photo_client.dart';
import 'package:flight_radar_companion/ui/aircraft_card.dart';

void main() {
  testWidgets('card shows callsign, route, distance, and an emergency badge',
      (tester) async {
    final photos = PhotoClient(MockClient((_) async => http.Response('{"photos":[]}', 200)));
    const a = Aircraft(
      callsign: 'BAW117', type: 'A388', lat: 51.5, lon: -0.45,
      altFt: 35000, gsKt: 450, onGround: false, squawk: 7700,
      registration: 'G-XLEA', origin: 'EGLL', dest: 'KJFK',
      hex: '40612a', desc: 'Airbus A380-841', isEmergency: true, distKm: 8.0,
    );
    await tester.pumpWidget(MaterialApp(
        home: Scaffold(body: AircraftCard(aircraft: a, photos: photos))));
    await tester.pump(); // let the photo future resolve to a miss (placeholder)

    expect(find.text('BAW117'), findsOneWidget);
    expect(find.text('EGLL → KJFK'), findsOneWidget);
    expect(find.textContaining('G-XLEA'), findsOneWidget);
    expect(find.text('EMG'), findsOneWidget);     // emergency badge
    expect(find.byIcon(Icons.flight), findsOneWidget); // placeholder on photo miss
  });

  testWidgets('military card shows MIL badge and no route when route absent',
      (tester) async {
    final photos = PhotoClient(MockClient((_) async => http.Response('{"photos":[]}', 200)));
    const a = Aircraft(
      callsign: 'RRR2745', type: 'A400', lat: 51, lon: -1, altFt: 8000, gsKt: 300,
      onGround: false, hex: '43c123', desc: 'Airbus A400M', isMilitary: true, distKm: 20.0,
    );
    await tester.pumpWidget(MaterialApp(
        home: Scaffold(body: AircraftCard(aircraft: a, photos: photos))));
    await tester.pump();

    expect(find.text('RRR2745'), findsOneWidget);
    expect(find.text('MIL'), findsOneWidget);
    expect(find.textContaining('→'), findsNothing); // no route line
  });
}
```

- [ ] **Step 2: Run, verify failure**

Run: `flutter test test/aircraft_card_test.dart`
Expected: FAIL — `aircraft_card.dart` missing.

- [ ] **Step 3: Implement**

Create `lib/ui/aircraft_card.dart`:

```dart
import 'package:flutter/material.dart';
import '../data/aircraft.dart';
import '../data/photo_client.dart';

/// One aircraft row: photo, callsign + badges, type/distance, route, attribution.
/// The photo is looked up lazily from [photos] (foreground only).
class AircraftCard extends StatelessWidget {
  final Aircraft aircraft;
  final PhotoClient photos;
  const AircraftCard({super.key, required this.aircraft, required this.photos});

  bool get _hasRoute =>
      (aircraft.origin ?? '').isNotEmpty &&
      (aircraft.dest ?? '').isNotEmpty &&
      aircraft.origin != aircraft.dest;

  @override
  Widget build(BuildContext context) {
    final cs = aircraft.callsign.isEmpty ? '------' : aircraft.callsign;
    final subtitle = aircraft.desc.isNotEmpty ? aircraft.desc : aircraft.type;
    final dist = aircraft.distKm == null ? '' : ' · ${aircraft.distKm!.round()} km';
    final reg = (aircraft.registration ?? '');

    return Card(
      margin: const EdgeInsets.symmetric(horizontal: 12, vertical: 6),
      child: Padding(
        padding: const EdgeInsets.all(8),
        child: Row(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            _PhotoBox(reg: reg, hex: aircraft.hex, photos: photos),
            const SizedBox(width: 12),
            Expanded(
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Row(children: [
                    Text(cs, style: const TextStyle(fontWeight: FontWeight.bold, fontSize: 16)),
                    const SizedBox(width: 8),
                    if (aircraft.isEmergency) _badge('EMG', Colors.red),
                    if (aircraft.isMilitary) _badge('MIL', Colors.green.shade700),
                  ]),
                  Text('$subtitle$dist'),
                  if (_hasRoute) Text('${aircraft.origin} → ${aircraft.dest}'),
                  if (reg.isNotEmpty) Text(reg),
                ],
              ),
            ),
          ],
        ),
      ),
    );
  }

  Widget _badge(String text, Color color) => Container(
        padding: const EdgeInsets.symmetric(horizontal: 6, vertical: 1),
        decoration: BoxDecoration(color: color, borderRadius: BorderRadius.circular(4)),
        child: Text(text, style: const TextStyle(color: Colors.white, fontSize: 11, fontWeight: FontWeight.bold)),
      );
}

class _PhotoBox extends StatelessWidget {
  final String reg;
  final String hex;
  final PhotoClient photos;
  const _PhotoBox({required this.reg, required this.hex, required this.photos});

  @override
  Widget build(BuildContext context) {
    return SizedBox(
      width: 80,
      height: 60,
      child: FutureBuilder<PhotoRef?>(
        future: photos.lookup(reg: reg, hex: hex),
        builder: (context, snap) {
          final photo = snap.data;
          if (photo == null) {
            return Container(
              color: Colors.black12,
              alignment: Alignment.center,
              child: const Icon(Icons.flight, color: Colors.black38),
            );
          }
          return Column(
            crossAxisAlignment: CrossAxisAlignment.stretch,
            children: [
              Expanded(
                child: Image.network(photo.thumbUrl, fit: BoxFit.cover,
                    errorBuilder: (_, __, ___) => Container(
                        color: Colors.black12,
                        child: const Icon(Icons.flight, color: Colors.black38))),
              ),
              Text('© ${photo.photographer} / planespotters.net',
                  style: const TextStyle(fontSize: 7), overflow: TextOverflow.ellipsis),
            ],
          );
        },
      ),
    );
  }
}
```

- [ ] **Step 4: Run, verify pass**

Run: `flutter test test/aircraft_card_test.dart`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add lib/ui/aircraft_card.dart test/aircraft_card_test.dart
git commit -m "feat(app): AircraftCard with photo, badges, route, attribution"
```

---

### Task 9: home_screen — list integration

**Files:** Modify `lib/ui/home_screen.dart`; verify `test/widget_test.dart` still passes.

Glue; verify by `flutter test` + `flutter analyze`.

- [ ] **Step 1: Add imports + a shared PhotoClient**

In `lib/ui/home_screen.dart`, add imports:

```dart
import '../data/photo_client.dart';
import 'aircraft_card.dart';
```

Add a field to `_HomeScreenState` (after `bool _running = false;`):

```dart
  final _photos = PhotoClient();
```

- [ ] **Step 2: Render the list under the status header**

Replace the `build` method's `body` `Column` `children` (currently the three `_row(...)`, `Spacer`, and the button) with a status header + the list. Replace the whole `Padding(...)` body with:

```dart
      body: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          Padding(
            padding: const EdgeInsets.fromLTRB(24, 16, 24, 8),
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                _row('Device', _status.ble),
                _row('GPS', _status.fix),
                _row('Last packet', '${_status.count} aircraft'),
                const SizedBox(height: 12),
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
          const Divider(height: 1),
          Expanded(
            child: _status.aircraft.isEmpty
                ? const Center(
                    child: Text('Start feeding to see nearby aircraft',
                        style: TextStyle(color: Colors.black54)))
                : ListView.builder(
                    itemCount: _status.aircraft.length,
                    itemBuilder: (context, i) =>
                        AircraftCard(aircraft: _status.aircraft[i], photos: _photos),
                  ),
          ),
        ],
      ),
```

(Keep the `_row` helper and everything else unchanged. The button text `'Start feeding device'` is preserved so `widget_test.dart` still passes.)

- [ ] **Step 3: Verify**

Run: `flutter test` → all pass (incl. the existing `widget_test.dart` which checks the title + 'Start feeding device'). Then `flutter analyze` → clean.

- [ ] **Step 4: Commit**

```bash
git add lib/ui/home_screen.dart
git commit -m "feat(app): home screen lists fed aircraft with photo cards"
```

---

### Task 10: Full verify + on-device acceptance

**Files:** none (verification only).

- [ ] **Step 1: Full suite + analyze**

Run: `flutter test`
Expected: PASS — all (21 existing + new model/parser/alerts/photo/card tests).

Run: `flutter analyze`
Expected: no issues.

- [ ] **Step 2: On-device (manual, requires a phone)**

Build/run release on a device (per the toolchain notes, iOS needs `--release` to run standalone):
`flutter run --release`

Acceptance checklist:
- Tap Start; grant permissions (incl. the notification prompt). The home screen
  fills with cards for nearby aircraft, each showing type/distance, route when
  known, registration, and a photo (or the airplane-icon placeholder).
- An emergency-squawk or military aircraft card shows the EMG/MIL badge.
- When such an aircraft enters the feed, a notification fires; a persistent
  aircraft does not re-notify every cycle; one that leaves and returns re-notifies.
  (If none is overhead, temporarily relax the predicate or feed near known
  military traffic to smoke-test, then revert.)
- Background: with the app backgrounded and feeding, an emergency/military
  arrival still notifies (Android foreground service; iOS keep-alive).
  - If iOS shows no notification, add the standard plugin delegate line to
    `ios/Runner/AppDelegate.swift`
    (`UNUserNotificationCenter.current().delegate = self as? UNUserNotificationCenterDelegate`
    is NOT needed for banners; modern plugin auto-registers — only investigate if
    the on-device test fails) and re-run.

---

## After implementation

Use superpowers:finishing-a-development-branch to verify tests, then merge + push per the established cadence. Note-only follow-up (carried): the docs (README/ARCHITECTURE/HARDWARE/CLAUDE.md) still describe the v2 wire / single RADIUS_NM and don't mention the detail enrichment, range presets/rim dots, or this phone viewer/push — a future docs-refresh pass.
