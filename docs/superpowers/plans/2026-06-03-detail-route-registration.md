# Detail Enrichment (route + registration + operator) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Show registration, operator (airline code), and route (origin→destination) in the device's detail view, on both the Wi-Fi and BLE paths (BLE wire bumped to v3); route comes from hexdb.io (free).

**Architecture:** New `Aircraft` string fields `registration`/`origin`/`dest`. Registration from airplanes.live `r`; route from hexdb.io (`callsign → "EGLL-KJFK"`), looked up lazily on Wi-Fi (firmware, cached) and prefetched on BLE (app, cached) then carried in a v3 wire record (48 B). Operator is derived free from the callsign (airline ICAO). Pure helpers (`parseHexdbRoute`, `airlineCode`) are TDD'd; the HTTP clients + render are verified on-device.

**Tech Stack:** C++ (PlatformIO native Unity), Dart (flutter_test), Python (bleak harness), hexdb.io REST.

---

## File Structure
- `src/render_core.h` — `parseHexdbRoute`, `airlineCode` (pure).
- `src/flight_core.h` — `Aircraft` gains `registration`/`origin`/`dest`; `parseNearest` reads `r`.
- `src/ble_core.h` — wire v3 (record 48, parse registration/origin/dest).
- `src/flight_ticker.ino` — hexdb route client + cache; `drawDetail` Reg/Op/Route.
- `test/test_core/test_main.cpp` — new + updated tests.
- `companion/lib/data/aircraft.dart` — Dart `Aircraft` gains the 3 fields + `copyWith`.
- `companion/lib/data/airplanes_client.dart` — `parseAircraft` reads `r`; `parseHexdbRoute` (Dart).
- `companion/lib/data/route_client.dart` — **new.** hexdb client + cache.
- `companion/lib/service/gateway_engine.dart` — enrich aircraft with routes before encode.
- `companion/lib/packet/ble_packet.dart` — encode v3.
- `companion/test/*.dart` — new + updated tests.
- `scripts/ble_send.py` — emit v3.

---

## Task 1: Pure helpers `parseHexdbRoute` + `airlineCode` (render_core)

**Files:** Modify `src/render_core.h`; Test `test/test_core/test_main.cpp`.

READ `src/render_core.h` (it includes `<string>`? confirm; add `#include <string>` if missing).

- [ ] **Step 1: Write the failing tests** (add before `void setUp`):
```cpp
void test_parse_hexdb_route(void) {
    auto r = parseHexdbRoute("EGLL-KJFK");
    TEST_ASSERT_EQUAL_STRING("EGLL", r.first.c_str());
    TEST_ASSERT_EQUAL_STRING("KJFK", r.second.c_str());
    auto multi = parseHexdbRoute("EGLL-LEMD-EGLL"); // first->last
    TEST_ASSERT_EQUAL_STRING("EGLL", multi.first.c_str());
    TEST_ASSERT_EQUAL_STRING("EGLL", multi.second.c_str());
    auto empty = parseHexdbRoute("");
    TEST_ASSERT_EQUAL_STRING("", empty.first.c_str());
    TEST_ASSERT_EQUAL_STRING("", empty.second.c_str());
    auto one = parseHexdbRoute("EGLL");
    TEST_ASSERT_EQUAL_STRING("EGLL", one.first.c_str());
    TEST_ASSERT_EQUAL_STRING("EGLL", one.second.c_str());
}

void test_airline_code(void) {
    TEST_ASSERT_EQUAL_STRING("BAW", airlineCode("BAW117").c_str());
    TEST_ASSERT_EQUAL_STRING("DLH", airlineCode("DLH4AB").c_str());
    TEST_ASSERT_EQUAL_STRING("", airlineCode("N12345").c_str()); // digits, not an airline
    TEST_ASSERT_EQUAL_STRING("", airlineCode("AB").c_str());     // too short
    TEST_ASSERT_EQUAL_STRING("", airlineCode("").c_str());
}
```
Register: `RUN_TEST(test_parse_hexdb_route); RUN_TEST(test_airline_code);`

- [ ] **Step 2: Run** `/opt/homebrew/bin/pio test -d /Users/denyskolomiiets/flight-radar-esp32 -e native -f test_core` → FAIL (undefined).

- [ ] **Step 3: Add to `src/render_core.h`** (after the existing helpers; needs `<string>`, `<utility>`):
```cpp
// Split a hexdb.io route string ("EGLL-KJFK", possibly multi-leg) into
// (origin, dest) = (first, last) ICAO codes. Empty pair on empty input.
inline std::pair<std::string, std::string> parseHexdbRoute(const std::string& route) {
    if (route.empty()) return {"", ""};
    size_t first = route.find('-');
    if (first == std::string::npos) return {route, route};
    std::string origin = route.substr(0, first);
    size_t last = route.find_last_of('-');
    std::string dest = route.substr(last + 1);
    return {origin, dest};
}

// Airline ICAO code = first 3 chars of an airline callsign (letters). "" for
// tail-number callsigns (digits) or short callsigns.
inline std::string airlineCode(const std::string& callsign) {
    if (callsign.size() < 3) return "";
    for (int i = 0; i < 3; i++) {
        char c = callsign[i];
        if (c < 'A' || c > 'Z') return "";
    }
    return callsign.substr(0, 3);
}
```
Add `#include <utility>` and `#include <string>` to the top of `render_core.h` if not already present.

- [ ] **Step 4: Run** → PASS. **Step 5: Commit**
```bash
git add src/render_core.h test/test_core/test_main.cpp
git commit -m "feat: parseHexdbRoute + airlineCode pure helpers with tests"
```

---

## Task 2: Firmware `Aircraft` fields + `parseNearest` registration

**Files:** Modify `src/flight_core.h`; Test `test/test_core/test_main.cpp`.

READ `src/flight_core.h` (Aircraft struct; parseNearest filter + loop).

- [ ] **Step 1: Failing test** (before `void setUp`):
```cpp
void test_parse_nearest_registration(void) {
    const char* json = "{\"ac\":[{\"flight\":\"BAW1\",\"r\":\"G-XLEA\",\"lat\":0.0,\"lon\":0.1}]}";
    auto out = parseNearest(json, 0.0, 0.0, 5);
    TEST_ASSERT_EQUAL_STRING("G-XLEA", out[0].registration.c_str());
    auto out2 = parseNearest("{\"ac\":[{\"flight\":\"X\",\"lat\":0.0,\"lon\":0.1}]}", 0.0, 0.0, 5);
    TEST_ASSERT_EQUAL_STRING("", out2[0].registration.c_str());
}
```
Register it.

- [ ] **Step 2: Run** → FAIL (no `registration` member).

- [ ] **Step 3: Add fields + parse**. In the `Aircraft` struct (after `squawk`):
```cpp
    std::string registration; // tail number ("" if missing)
    std::string origin;       // route origin ICAO ("" if unknown)
    std::string dest;         // route dest ICAO ("" if unknown)
```
In `parseNearest` filter block: `filter["ac"][0]["r"] = true;`
In the loop, after the squawk line: `ac.registration = trimStr(a["r"].as<const char*>());`
(`trimStr` already exists and returns "" for null.)

- [ ] **Step 4: Run** → PASS. **Step 5: Commit**
```bash
git add src/flight_core.h test/test_core/test_main.cpp
git commit -m "feat: Aircraft registration/origin/dest fields; parse registration on Wi-Fi"
```

---

## Task 3: BLE wire format v3 (ble_core)

**Files:** Modify `src/ble_core.h`; Test `test/test_core/test_main.cpp`.

READ `src/ble_core.h` (constants, `parseBlePacket` loop, `bleField`) and the test helpers (`bleAddRecord`, `blePutField`).

- [ ] **Step 1: Update `bleAddRecord` to v3 + add test.** Append three string fields to the helper (optional, default ""):
```cpp
static void bleAddRecord(std::vector<uint8_t>& v, const char* cs, const char* ty,
                         float lat, float lon, int32_t alt, int16_t gs, uint8_t flags,
                         int16_t track = 0, uint16_t squawk = 0,
                         const char* reg = "", const char* origin = "", const char* dest = "") {
    blePutField(v, cs, 8); blePutField(v, ty, 4);
    blePutF32(v, lat); blePutF32(v, lon);
    blePutI32(v, alt); blePutI16(v, gs);
    v.push_back(flags); v.push_back(0);
    blePutI16(v, track);
    uint8_t b[2]; std::memcpy(b, &squawk, 2); v.insert(v.end(), b, b + 2);
    blePutField(v, reg, 8); blePutField(v, origin, 4); blePutField(v, dest, 4);
}
```
Add the v3 test:
```cpp
void test_ble_v3_route_registration(void) {
    std::vector<uint8_t> v = bleHeader(1, 48.0f, 11.0f);
    bleAddRecord(v, "BAW1", "A320", 48.1f, 11.0f, 35000, 450, BLE_FLAG_ALT_VALID,
                 0, 0, "G-XLEA", "EGLL", "KJFK");
    BlePacket p = parseBlePacket(v.data(), v.size(), 5);
    TEST_ASSERT_TRUE(p.ok);
    TEST_ASSERT_EQUAL_STRING("G-XLEA", p.aircraft[0].registration.c_str());
    TEST_ASSERT_EQUAL_STRING("EGLL", p.aircraft[0].origin.c_str());
    TEST_ASSERT_EQUAL_STRING("KJFK", p.aircraft[0].dest.c_str());
}
```
Register it. NOTE: `bleHeader` uses `BLE_VERSION` (→3 in Step 3); existing tests now build 48-byte records via the defaulted "" fields and still pass. `test_ble_count_overflow` uses 17 > new max 10 → still overflows.

- [ ] **Step 2: Run** → FAIL.

- [ ] **Step 3: Bump ble_core.h to v3**:
```cpp
constexpr uint8_t BLE_VERSION      = 3;
constexpr size_t  BLE_MAX_AIRCRAFT = 10;
constexpr size_t  BLE_RECORD_SIZE  = 48;
```
(`BLE_MAX_PACKET` recomputes to 492.) In `parseBlePacket`'s record loop, after the squawk read and before push_back, add:
```cpp
        ac.registration = bleField(r + 32, 8);
        ac.origin       = bleField(r + 40, 4);
        ac.dest         = bleField(r + 44, 4);
```
(`bleField` already trims space-padding to "".)

- [ ] **Step 4: Run** → PASS (new v3 test + all existing). **Step 5: Commit**
```bash
git add src/ble_core.h test/test_core/test_main.cpp
git commit -m "feat: BLE wire format v3 — registration + route (record 48B, cap 10)"
```

---

## Task 4: Firmware hexdb route client + detail render

**Files:** Modify `src/flight_ticker.ino`. No host test (HTTP glue; the parse is tested). READ `pollApi()` (the WiFiClientSecure HTTPS pattern), `drawDetail()` (the `sub` subtitle build + the field layout), and the globals/`g_view`.

- [ ] **Step 1: Add a global route cache + lookup function**. After the includes/globals, add:
```cpp
#include <map>
std::map<std::string, std::pair<std::string, std::string>> g_routeCache; // callsign -> (origin,dest)

// Blocking hexdb.io route lookup; caches by callsign. Returns (origin,dest) or
// ("","") on failure. Only call when WiFi is connected.
std::pair<std::string, std::string> lookupRoute(const std::string& callsign) {
    if (callsign.empty()) return {"", ""};
    auto it = g_routeCache.find(callsign);
    if (it != g_routeCache.end()) return it->second;
    std::pair<std::string, std::string> result{"", ""};
    char url[96];
    std::snprintf(url, sizeof(url), "https://hexdb.io/api/v1/route/icao/%s", callsign.c_str());
    WiFiClientSecure client; client.setInsecure();
    HTTPClient http; http.begin(client, url);
    http.setUserAgent("flight-ticker-esp32");
    http.setConnectTimeout(6000); http.setTimeout(6000);
    if (http.GET() == 200) {
        JsonDocument doc;
        if (!deserializeJson(doc, http.getString()) && doc["route"].is<const char*>()) {
            result = parseHexdbRoute(std::string(doc["route"].as<const char*>()));
        }
    }
    http.end();
    g_routeCache[callsign] = result; // cache even empties to avoid re-hitting
    return result;
}
```

- [ ] **Step 2: Resolve + render route in `drawDetail`**. At the top of `drawDetail`, after the current aircraft `ac` is selected, resolve origin/dest (from the packet on BLE, else hexdb on Wi-Fi):
```cpp
    std::string rOrigin = ac.origin, rDest = ac.dest;
    if (rOrigin.empty() && WiFi.status() == WL_CONNECTED) {
        auto rt = lookupRoute(ac.callsign);
        rOrigin = rt.first; rDest = rt.second;
    }
```
Then, in the detail layout (after the existing lines — distance/alt/speed), add three compact lines, each only when non-empty. Use the existing sprite `fb`, a small font (font 2), and place them below the current content (pick y-coords that fit the round panel — read the existing y positions and continue downward, e.g. y≈150/168/186). Render:
```cpp
    fb.setTextDatum(TC_DATUM);
    fb.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    if (!ac.registration.empty())
        fb.drawString(("Reg " + ac.registration).c_str(), CX, 150, 2);
    std::string op = airlineCode(ac.callsign);
    if (!op.empty())
        fb.drawString(("Op " + op).c_str(), CX, 168, 2);
    if (!rOrigin.empty())
        fb.drawString((rOrigin + " > " + rDest).c_str(), CX, 186, 2);
```
NOTE: match the real variable names + existing y-layout (read `drawDetail` first; the y values above are a starting guess — adjust so nothing overlaps the existing fields or the bottom indicator). Keep it lean.

- [ ] **Step 3: Verify compiles**: `/opt/homebrew/bin/pio run -d /Users/denyskolomiiets/flight-radar-esp32 -e esp32-s3` → SUCCESS. Then `pio test -e native -f test_core` → still passes (shared headers untouched).

- [ ] **Step 4: Commit**
```bash
git add src/flight_ticker.ino
git commit -m "feat: hexdb route lookup + Reg/Op/Route in the detail view"
```

---

## Task 5: App `Aircraft` fields + `copyWith` + `parseAircraft` registration + `parseHexdbRoute`

**Files:** Modify `companion/lib/data/aircraft.dart`, `companion/lib/data/airplanes_client.dart`; Test `companion/test/airplanes_client_test.dart`.

- [ ] **Step 1: Failing tests** (append to airplanes_client_test.dart main()):
```dart
  test('parseAircraft extracts registration', () {
    final a = parseAircraft('{"ac":[{"flight":"BAW1","r":"G-XLEA","lat":0.0,"lon":0.1}]}', 0, 0).single;
    expect(a.registration, 'G-XLEA');
    final b = parseAircraft('{"ac":[{"flight":"X","lat":0.0,"lon":0.1}]}', 0, 0).single;
    expect(b.registration, isNull);
  });

  test('parseHexdbRoute splits ICAO route', () {
    expect(parseHexdbRoute('EGLL-KJFK'), ('EGLL', 'KJFK'));
    expect(parseHexdbRoute('EGLL-LEMD-EGLL'), ('EGLL', 'EGLL'));
    expect(parseHexdbRoute(''), ('', ''));
  });
```

- [ ] **Step 2: Run** `cd companion && flutter test test/airplanes_client_test.dart` → FAIL.

- [ ] **Step 3: Implement.** In `companion/lib/data/aircraft.dart`, add three optional nullable fields to `Aircraft` + the const constructor (no `required`):
```dart
  final String? registration;
  final String? origin;
  final String? dest;
```
Add `this.registration, this.origin, this.dest,` to the constructor. Add a `copyWith` for route enrichment:
```dart
  Aircraft copyWith({String? origin, String? dest}) => Aircraft(
        callsign: callsign, type: type, lat: lat, lon: lon, altFt: altFt,
        gsKt: gsKt, onGround: onGround, track: track, squawk: squawk,
        registration: registration, origin: origin ?? this.origin, dest: dest ?? this.dest,
      );
```
In `companion/lib/data/airplanes_client.dart`:
- add a top-level pure function:
```dart
/// Split a hexdb.io route ("EGLL-KJFK") into (origin, dest) = (first, last) ICAO.
(String, String) parseHexdbRoute(String route) {
  if (route.isEmpty) return ('', '');
  final first = route.indexOf('-');
  if (first < 0) return (route, route);
  return (route.substring(0, first), route.substring(route.lastIndexOf('-') + 1));
}
```
- in `parseAircraft`, after `gsKt`/track/squawk, add `final String? registration = (item['r'] is String) ? item['r'] as String : null;` and pass `registration: registration` into `Aircraft(...)`.

- [ ] **Step 4: Run** `cd companion && flutter test` → all pass. `flutter analyze` → clean. **Step 5: Commit**
```bash
git add companion/lib/data/aircraft.dart companion/lib/data/airplanes_client.dart companion/test/airplanes_client_test.dart
git commit -m "feat(companion): Aircraft registration/origin/dest + copyWith + parseHexdbRoute"
```

---

## Task 6: App wire format v3 encoder (ble_packet.dart)

**Files:** Modify `companion/lib/packet/ble_packet.dart`; Test `companion/test/ble_packet_test.dart`.

The v3 contract (matches firmware): version 3, record 48 B, registration[32:40] ascii, origin[40:44], dest[44:48]; max 15→10.

- [ ] **Step 1: Update `_ac` + existing tests + add v3 test.** In `ble_packet_test.dart`, update `_ac` to accept reg/origin/dest:
```dart
Aircraft _ac({
  String cs = 'AAA', String ty = 'A320',
  double lat = 0, double lon = 0, int? alt = 1000, int? gs = 300, bool ground = false,
  double? track, int? squawk, String? registration, String? origin, String? dest,
}) => Aircraft(callsign: cs, type: ty, lat: lat, lon: lon, altFt: alt, gsKt: gs,
    onGround: ground, track: track, squawk: squawk,
    registration: registration, origin: origin, dest: dest);
```
Update existing record-size/cap assertions: `12 + 32` → `12 + 48`; cap test → cap 10 (`bytes[3] == 10`, length `12 + 10*48`, build >10 aircraft). Add v3 test:
```dart
  test('encodePacket v3 writes registration + route', () {
    final bytes = encodePacket(48.0, 11.0, [_ac(cs: 'BAW1', registration: 'G-XLEA', origin: 'EGLL', dest: 'KJFK')]);
    expect(bytes[2], 3);
    expect(bytes.length, 12 + 48);
    expect(String.fromCharCodes(bytes.sublist(44, 48)), 'KJFK');
    expect(String.fromCharCodes(bytes.sublist(40, 44)), 'EGLL');
    expect(String.fromCharCodes(bytes.sublist(32, 40)), 'G-XLEA  '); // 8, space-padded
  });
```

- [ ] **Step 2: Run** `cd companion && flutter test test/ble_packet_test.dart` → FAIL.

- [ ] **Step 3: Bump `ble_packet.dart` to v3**: `bleVersion = 3;`, `bleMaxAircraft = 10;`, `bleRecordSize = 48;` (bleMaxPacket recomputes to 492). In `encodePacket`'s loop, after the squawk write, append:
```dart
    _writeField(out, base + 32, 8, a.registration ?? '');
    _writeField(out, base + 40, 4, a.origin ?? '');
    _writeField(out, base + 44, 4, a.dest ?? '');
```

- [ ] **Step 4: Run** `cd companion && flutter test` → all pass; `flutter analyze` → clean. **Step 5: Commit**
```bash
git add companion/lib/packet/ble_packet.dart companion/test/ble_packet_test.dart
git commit -m "feat(companion): BLE wire v3 encoder — registration + route (record 48B, cap 10)"
```
NOTE: `bleMaxAircraft` 16→10 also tightens `parseAircraft`'s trim; update the airplanes_client "caps to 15"→"caps to 10" test + comment (consequence of the constant change), and commit them with this task.

---

## Task 7: App hexdb route client + gateway prefetch

**Files:** Create `companion/lib/data/route_client.dart`; Modify `companion/lib/service/gateway_engine.dart`. No host test (HTTP glue; `parseHexdbRoute` is tested). 

- [ ] **Step 1: Create `companion/lib/data/route_client.dart`**:
```dart
import 'package:http/http.dart' as http;
import 'dart:convert';
import 'airplanes_client.dart' show parseHexdbRoute;

/// Looks up a flight's route from hexdb.io and caches it per callsign.
class RouteClient {
  final http.Client _http;
  final Map<String, (String, String)> _cache = {};
  RouteClient([http.Client? client]) : _http = client ?? http.Client();

  /// (origin, dest) ICAO for a callsign; ('','') on unknown/error. Cached.
  Future<(String, String)> lookup(String callsign) async {
    if (callsign.isEmpty) return ('', '');
    final hit = _cache[callsign];
    if (hit != null) return hit;
    (String, String) result = ('', '');
    try {
      final resp = await _http.get(
        Uri.parse('https://hexdb.io/api/v1/route/icao/$callsign'),
        headers: {'User-Agent': 'flight-radar-companion'},
      );
      if (resp.statusCode == 200) {
        final m = json.decode(resp.body);
        if (m is Map && m['route'] is String) result = parseHexdbRoute(m['route'] as String);
      }
    } catch (_) {/* leave empty */}
    _cache[callsign] = result; // cache empties too
    return result;
  }
}
```

- [ ] **Step 2: Enrich aircraft with routes in the engine**. In `companion/lib/service/gateway_engine.dart`, add a `RouteClient` field (`final RouteClient _routes = RouteClient();`, import it), and in `_cycle`, after `fetchNearby` returns `aircraft` and before `encodePacket`, enrich each with its route:
```dart
      final enriched = <Aircraft>[];
      for (final a in aircraft) {
        final (o, d) = await _routes.lookup(a.callsign);
        enriched.add(o.isEmpty ? a : a.copyWith(origin: o, dest: d));
      }
      final packet = encodePacket(fix.lat, fix.lon, enriched);
```
(Replace the `encodePacket(fix.lat, fix.lon, aircraft)` call with the enriched version. Routes are cached, so only new callsigns hit hexdb. Match the real variable names from the existing `_cycle`.)

- [ ] **Step 3: Verify** `cd companion && flutter analyze && flutter test` → clean + all pass (no new host test; this is glue, but confirm nothing broke).

- [ ] **Step 4: Commit**
```bash
git add companion/lib/data/route_client.dart companion/lib/service/gateway_engine.dart
git commit -m "feat(companion): hexdb route client + per-cycle route enrichment for BLE"
```

---

## Task 8: `ble_send.py` v3 + on-device verification

**Files:** Modify `scripts/ble_send.py`.

- [ ] **Step 1: Update to v3.** READ `scripts/ble_send.py`. Change the header version byte `2`→`3`. Update `_record` to append three space-padded ASCII fields (registration[8], origin[4], dest[4]) after the `<hH` track/squawk block:
```python
def _record(cs, ty, lat, lon, alt_ft, gs_kt, flags, track=0, squawk=0, reg="", origin="", dest="") -> bytes:
    return (_field(cs, 8) + _field(ty, 4)
            + struct.pack("<ffihBB", lat, lon, alt_ft, gs_kt, flags, 0)
            + struct.pack("<hH", track, squawk)
            + _field(reg, 8) + _field(origin, 4) + _field(dest, 4))
```
Update the sample `aircraft` tuples to add reg/origin/dest, e.g. for one: `..., 270, 1200, "G-XLEA", "EGLL", "KJFK"`. Ensure the loop unpacks the longer tuples (it uses `_record(*a)`, so just extend the tuples).

- [ ] **Step 2: Byte-check** (no device): a 3-aircraft v3 packet must be `12 + 3*48 = 156` bytes and `bytes[2] == 3`. Verify via a throwaway `python3 -` import (no scratch file committed). Report length + version.

- [ ] **Step 3: Compile** `/opt/homebrew/bin/pio run -d /Users/denyskolomiiets/flight-radar-esp32 -e esp32-s3` → SUCCESS; `pio test -e native -f test_core` → passes.

- [ ] **Step 4: Commit**
```bash
git add scripts/ble_send.py
git commit -m "test: ble_send v3 packets with registration + route"
```

- [ ] **Step 5: On-device** (controller drives — needs ESP32 + the bad-SSID/WiFi flow): flash, then Wi-Fi path → tap a real aircraft → Reg/Op/Route appear (route from hexdb). Then bad-SSID + `ble_send.py` v3 → tap the injected aircraft → Reg/Op/Route from the packet. Restore production config after.

---

## Done criteria
- `pio test -e native -f test_core` + `cd companion && flutter test` green (incl. v3 round-trip, `parseHexdbRoute`, `airlineCode`, registration parse).
- `pio run -e esp32-s3` compiles; `ble_send.py` emits v3 (156 B for 3 aircraft).
- On device (both paths): tapping a blip shows its registration, operator (airline code), and route origin→destination — route from hexdb.io on Wi-Fi, from the v3 packet on BLE.
