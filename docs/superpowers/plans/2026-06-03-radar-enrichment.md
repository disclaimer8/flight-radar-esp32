# Radar Enrichment Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add heading vectors, altitude-based blip color, and emergency-squawk highlighting to the radar, on both the Wi-Fi and BLE data paths (BLE wire format bumped to v2 to carry the two new fields `track` + `squawk`).

**Architecture:** Two new `Aircraft` fields (`track`, `squawk`) flow from airplanes.live through `parseNearest` (Wi-Fi) and `parseAircraft` (app), and across BLE via a v2 wire record (32 B, version 2, max 15 aircraft). Three new pure host-tested render helpers (`vectorEnd`, `altBand`, `isEmergencySquawk`) feed the firmware's `drawRadar`. Pure functions are TDD'd; the `.ino` render and `ble_send.py` are verified on-device.

**Tech Stack:** C++ (PlatformIO native Unity), Dart (flutter_test), Python (bleak harness).

---

## File Structure

- `src/flight_core.h` — `Aircraft` gains `track`/`squawk`; `parseNearest` extracts them.
- `src/render_core.h` — new pure helpers `vectorEnd`, `altBand`, `isEmergencySquawk`.
- `src/ble_core.h` — wire v2 (version/record/max/flags, parse track/squawk).
- `src/flight_ticker.ino` — `drawRadar`/`drawDetail` render the three features.
- `test/test_core/test_main.cpp` — new + updated Unity tests.
- `companion/lib/data/aircraft.dart` — Dart `Aircraft` gains `track`/`squawk`.
- `companion/lib/data/airplanes_client.dart` — `parseAircraft` extracts them.
- `companion/lib/packet/ble_packet.dart` — encode v2.
- `companion/test/*.dart` — new + updated Dart tests.
- `scripts/ble_send.py` — emit v2 packets (incl. a 7700 emergency sample).

---

## Task 1: Pure render helpers (render_core)

**Files:**
- Modify: `src/render_core.h`
- Test: `test/test_core/test_main.cpp`

READ `src/render_core.h` first to confirm the `ScreenPoint` definition (it has integer `x`/`y`) and the include of `<cmath>`.

- [ ] **Step 1: Write the failing tests**

Add before `void setUp` in `test/test_core/test_main.cpp`:

```cpp
void test_vector_end_cardinals(void) {
    // North-up: heading 0 = straight up (-y); 90 = right (+x).
    ScreenPoint up = vectorEnd(ScreenPoint{100, 100}, 0.0, 10.0);
    TEST_ASSERT_EQUAL_INT(100, up.x);
    TEST_ASSERT_EQUAL_INT(90, up.y);
    ScreenPoint right = vectorEnd(ScreenPoint{100, 100}, 90.0, 10.0);
    TEST_ASSERT_EQUAL_INT(110, right.x);
    TEST_ASSERT_EQUAL_INT(100, right.y);
}

void test_alt_band(void) {
    TEST_ASSERT_EQUAL_INT(0, altBand(NAN, false));   // unknown
    TEST_ASSERT_EQUAL_INT(0, altBand(5000, true));   // on ground
    TEST_ASSERT_EQUAL_INT(1, altBand(1500, false));  // <3000
    TEST_ASSERT_EQUAL_INT(2, altBand(8000, false));  // 3k-10k
    TEST_ASSERT_EQUAL_INT(3, altBand(20000, false)); // 10k-25k
    TEST_ASSERT_EQUAL_INT(4, altBand(35000, false)); // 25k-40k
    TEST_ASSERT_EQUAL_INT(5, altBand(45000, false)); // >40k
}

void test_is_emergency_squawk(void) {
    TEST_ASSERT_TRUE(isEmergencySquawk(7500));
    TEST_ASSERT_TRUE(isEmergencySquawk(7600));
    TEST_ASSERT_TRUE(isEmergencySquawk(7700));
    TEST_ASSERT_FALSE(isEmergencySquawk(1200));
    TEST_ASSERT_FALSE(isEmergencySquawk(0));
}
```

Register in `main()`:
```cpp
    RUN_TEST(test_vector_end_cardinals);
    RUN_TEST(test_alt_band);
    RUN_TEST(test_is_emergency_squawk);
```

- [ ] **Step 2: Run to verify failure**

Run: `/opt/homebrew/bin/pio test -e native -f test_core`
Expected: FAIL — helpers undefined.

- [ ] **Step 3: Add the helpers to `src/render_core.h`**

Add (after the existing helpers; `ScreenPoint` and `<cmath>` are already present):

```cpp
// Endpoint of a fixed-length line from `from` along `headingDeg` (north-up:
// 0 = up/north, 90 = right/east). Used to draw aircraft heading vectors.
inline ScreenPoint vectorEnd(ScreenPoint from, double headingDeg, double length) {
    double r = headingDeg * M_PI / 180.0;
    return ScreenPoint{
        (int)std::lround(from.x + length * std::sin(r)),
        (int)std::lround(from.y - length * std::cos(r)),
    };
}

// Altitude band index for blip color: 0 ground/unknown, 1 <3k, 2 3-10k,
// 3 10-25k, 4 25-40k, 5 >40k (feet).
inline int altBand(double altFt, bool onGround) {
    if (onGround || std::isnan(altFt)) return 0;
    if (altFt < 3000)  return 1;
    if (altFt < 10000) return 2;
    if (altFt < 25000) return 3;
    if (altFt < 40000) return 4;
    return 5;
}

// Emergency transponder codes: 7500 hijack, 7600 radio fail, 7700 general.
inline bool isEmergencySquawk(int code) {
    return code == 7500 || code == 7600 || code == 7700;
}
```

- [ ] **Step 4: Run to verify pass**

Run: `/opt/homebrew/bin/pio test -e native -f test_core`
Expected: PASS (3 new tests + all existing).

- [ ] **Step 5: Commit**

```bash
git add src/render_core.h test/test_core/test_main.cpp
git commit -m "feat: add vectorEnd, altBand, isEmergencySquawk render helpers with tests"
```

---

## Task 2: `Aircraft` fields + `parseNearest` extraction (flight_core)

**Files:**
- Modify: `src/flight_core.h`
- Test: `test/test_core/test_main.cpp`

READ `src/flight_core.h`: the `Aircraft` struct and the `parseNearest` JSON filter + extraction loop.

- [ ] **Step 1: Write the failing test**

Add before `void setUp`:

```cpp
void test_parse_nearest_track_squawk(void) {
    const char* json =
        "{\"ac\":["
        "{\"flight\":\"ABC\",\"t\":\"A320\",\"lat\":0.0,\"lon\":0.1,\"alt_baro\":10000,"
        "\"gs\":300,\"track\":275.4,\"squawk\":\"7700\"}"
        "]}";
    auto out = parseNearest(json, 0.0, 0.0, 5);
    TEST_ASSERT_EQUAL_UINT32(1, out.size());
    TEST_ASSERT_FLOAT_WITHIN(0.1, 275.4, out[0].track);
    TEST_ASSERT_EQUAL_INT(7700, out[0].squawk);
    // Missing track/squawk -> defaults (NAN track, 0 squawk).
    const char* json2 = "{\"ac\":[{\"flight\":\"X\",\"lat\":0.0,\"lon\":0.1}]}";
    auto out2 = parseNearest(json2, 0.0, 0.0, 5);
    TEST_ASSERT_TRUE(std::isnan(out2[0].track));
    TEST_ASSERT_EQUAL_INT(0, out2[0].squawk);
}
```

Register: `RUN_TEST(test_parse_nearest_track_squawk);`

- [ ] **Step 2: Run to verify failure**

Run: `/opt/homebrew/bin/pio test -e native -f test_core`
Expected: FAIL — `Aircraft` has no `track`/`squawk`.

- [ ] **Step 3: Add fields + extraction**

In `src/flight_core.h`, add to the `Aircraft` struct (after `gsKt`):
```cpp
    double track = NAN;    // true track degrees; NAN if missing
    int    squawk = 0;     // transponder code (e.g. 7700); 0 if missing
```
In `parseNearest`, add to the JSON filter block:
```cpp
    filter["ac"][0]["track"] = true;
    filter["ac"][0]["squawk"] = true;
```
In the extraction loop, after the `gs` line and before the skip/`push_back`, add:
```cpp
        if (a["track"].is<double>()) ac.track = a["track"].as<double>();
        if (a["squawk"].is<const char*>()) ac.squawk = atoi(a["squawk"].as<const char*>());
```
(`atoi` needs `<cstdlib>` — add the include if not present.)

- [ ] **Step 4: Run to verify pass**

Run: `/opt/homebrew/bin/pio test -e native -f test_core`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/flight_core.h test/test_core/test_main.cpp
git commit -m "feat: parse track + squawk into Aircraft on the Wi-Fi path"
```

---

## Task 3: BLE wire format v2 (ble_core)

**Files:**
- Modify: `src/ble_core.h`
- Test: `test/test_core/test_main.cpp`

READ `src/ble_core.h` (constants, flags, `parseBlePacket` decode loop) and the BLE test helpers in `test_main.cpp` (`bleHeader`, `bleAddRecord`, `blePutI16`, etc.).

- [ ] **Step 1: Update the test helpers to v2 and write the new test**

In `test/test_core/test_main.cpp`, update `bleAddRecord` to write a 32-byte v2 record with optional track/squawk (defaults keep existing call sites valid), and add a v2 round-trip test.

Change the `bleAddRecord` helper signature + body to:
```cpp
static void bleAddRecord(std::vector<uint8_t>& v, const char* cs, const char* ty,
                         float lat, float lon, int32_t alt, int16_t gs, uint8_t flags,
                         int16_t track = 0, uint16_t squawk = 0) {
    blePutField(v, cs, 8); blePutField(v, ty, 4);
    blePutF32(v, lat); blePutF32(v, lon);
    blePutI32(v, alt); blePutI16(v, gs);
    v.push_back(flags); v.push_back(0);   // flags + pad
    blePutI16(v, track);
    uint8_t b[2]; std::memcpy(b, &squawk, 2); v.insert(v.end(), b, b + 2); // u16 squawk
}
```

Add the new test:
```cpp
void test_ble_v2_track_squawk(void) {
    std::vector<uint8_t> v = bleHeader(1, 48.0f, 11.0f);
    bleAddRecord(v, "DLH", "A320", 48.1f, 11.0f, 35000, 450,
                 BLE_FLAG_ALT_VALID | BLE_FLAG_GS_VALID | BLE_FLAG_TRACK_VALID | BLE_FLAG_SQUAWK_VALID,
                 287, 7700);
    BlePacket p = parseBlePacket(v.data(), v.size(), 5);
    TEST_ASSERT_TRUE(p.ok);
    TEST_ASSERT_EQUAL_UINT32(1, p.aircraft.size());
    TEST_ASSERT_FLOAT_WITHIN(0.5, 287.0, p.aircraft[0].track);
    TEST_ASSERT_EQUAL_INT(7700, p.aircraft[0].squawk);
    // Invalid flags -> track NAN, squawk 0.
    std::vector<uint8_t> v2 = bleHeader(1, 0.0f, 0.0f);
    bleAddRecord(v2, "X", "B738", 0.0f, 0.1f, 1000, 100, BLE_FLAG_ALT_VALID, 123, 1200);
    BlePacket q = parseBlePacket(v2.data(), v2.size(), 5);
    TEST_ASSERT_TRUE(std::isnan(q.aircraft[0].track));
    TEST_ASSERT_EQUAL_INT(0, q.aircraft[0].squawk);
}
```
Register: `RUN_TEST(test_ble_v2_track_squawk);`

NOTE: `bleHeader` uses `BLE_VERSION`, so it automatically writes version 2 once Step 3 bumps it. Existing BLE tests keep working: they call `bleAddRecord` without track/squawk (defaults), now producing 32-byte v2 records that `parseBlePacket` reads. `test_ble_count_overflow` uses count 17 > `BLE_MAX_AIRCRAFT` (now 15) — still overflows. Confirm all BLE tests still pass after Step 3.

- [ ] **Step 2: Run to verify failure**

Run: `/opt/homebrew/bin/pio test -e native -f test_core`
Expected: FAIL — `BLE_FLAG_TRACK_VALID`/`BLE_FLAG_SQUAWK_VALID` undefined and the parser doesn't read track/squawk.

- [ ] **Step 3: Bump ble_core.h to v2**

In `src/ble_core.h`:
- Change constants:
```cpp
constexpr uint8_t BLE_VERSION      = 2;
constexpr size_t  BLE_MAX_AIRCRAFT = 15;
constexpr size_t  BLE_RECORD_SIZE  = 32;
```
(`BLE_HEADER_SIZE` stays 12; `BLE_MAX_PACKET` is computed from these — leave its formula, it becomes 492.)
- Add flag constants (after the existing `BLE_FLAG_GS_VALID`):
```cpp
constexpr uint8_t BLE_FLAG_TRACK_VALID  = 0x08;
constexpr uint8_t BLE_FLAG_SQUAWK_VALID = 0x10;
```
- In `parseBlePacket`'s record loop, after reading `flags` (and setting onGround/altFt/gsKt) and before `push_back`, add:
```cpp
        int16_t track; uint16_t squawk;
        std::memcpy(&track,  r + 28, 2);
        std::memcpy(&squawk, r + 30, 2);
        ac.track  = (flags & BLE_FLAG_TRACK_VALID)  ? (double)track : NAN;
        ac.squawk = (flags & BLE_FLAG_SQUAWK_VALID) ? (int)squawk   : 0;
```
(The buffer length validation `len == BLE_HEADER_SIZE + count*BLE_RECORD_SIZE` already uses `BLE_RECORD_SIZE`, so it now expects 32-byte records automatically.)

- [ ] **Step 4: Run to verify pass**

Run: `/opt/homebrew/bin/pio test -e native -f test_core`
Expected: PASS — the new v2 test plus all existing BLE tests (now v2 via the updated helpers).

- [ ] **Step 5: Commit**

```bash
git add src/ble_core.h test/test_core/test_main.cpp
git commit -m "feat: BLE wire format v2 — carry track + squawk, record 32B, cap 15"
```

---

## Task 4: Firmware render — vectors, altitude color, emergency (flight_ticker.ino)

**Files:**
- Modify: `src/flight_ticker.ino`

No host test (Arduino render); verified by compile here and on-device in Task 7. READ `drawRadar` (the blips loop and the source-indicator block) and `drawDetail`.

- [ ] **Step 1: Add the altitude color table**

Near the top of `flight_ticker.ino` (after the `CX/CY/MAXR` constants), add:
```cpp
// Blip color per altBand() index: ground/unknown, <3k, 3-10k, 10-25k, 25-40k, >40k.
static const uint16_t kAltColors[6] = {
    TFT_DARKGREY, TFT_RED, TFT_ORANGE, TFT_GREENYELLOW, TFT_CYAN, TFT_BLUE
};
```

- [ ] **Step 2: Replace the blips loop in `drawRadar`**

Replace the existing `// blips` loop with:
```cpp
    // blips: colored by altitude, with a heading vector; nearest highlighted.
    bool blinkOn = (millis() / 500) % 2 == 0;
    bool anyEmergency = false;
    int  emergencyCode = 0;
    for (size_t i = 0; i < g_cache.size(); i++) {
        const Aircraft& ac = g_cache[i];
        double b = bearingDeg(g_centerLat, g_centerLon, ac.lat, ac.lon);
        ScreenPoint p = polarToXY(b, ac.distKm, rangeKm(), CX, CY, MAXR);

        uint16_t color = kAltColors[altBand(ac.altFt, ac.onGround)];
        bool emerg = isEmergencySquawk(ac.squawk);
        if (emerg) { anyEmergency = true; emergencyCode = ac.squawk; color = blinkOn ? TFT_RED : TFT_DARKGREY; }

        // heading vector
        if (!std::isnan(ac.track)) {
            ScreenPoint e = vectorEnd(p, ac.track, 10.0);
            fb.drawLine(p.x, p.y, e.x, e.y, color);
        }

        if (i == 0) {
            fb.fillCircle(p.x, p.y, 4, color);
            fb.drawCircle(p.x, p.y, 6, TFT_WHITE); // nearest ring
            std::string cs = ac.callsign.empty() ? "------" : ac.callsign;
            fb.setTextDatum(TL_DATUM);
            fb.setTextColor(TFT_WHITE, TFT_BLACK);
            fb.drawString(cs.c_str(), p.x + 8, p.y - 4, 2);
        } else {
            fb.fillCircle(p.x, p.y, 2, color);
        }
    }
```

- [ ] **Step 3: Add the emergency banner**

In `drawRadar`, just before the source-indicator block (the `fb.setTextDatum(BC_DATUM);` for W/B/NO-LINK), add:
```cpp
    if (anyEmergency && blinkOn) {
        char ebuf[20];
        snprintf(ebuf, sizeof(ebuf), "EMERGENCY %d", emergencyCode);
        fb.setTextDatum(TC_DATUM);
        fb.setTextColor(TFT_RED, TFT_BLACK);
        fb.drawString(ebuf, CX, CY - 40, 2);
    }
```

- [ ] **Step 4: (Optional) show track + squawk in `drawDetail`**

In `drawDetail`, the sub-line already shows type + compass. Append the squawk when present: find the `sub` string build and add after it (only if non-zero):
```cpp
    if (ac.squawk != 0) { sub += "  "; sub += std::to_string(ac.squawk); }
```
(Keep it minimal; the line is short.)

- [ ] **Step 5: Verify it compiles**

Run: `/opt/homebrew/bin/pio run -e esp32-s3`
Expected: `[SUCCESS]`. Fix any undefined symbol (confirm `TFT_GREENYELLOW` exists in TFT_eSPI — it does; `std::isnan`/`std::to_string` need `<cmath>`/`<string>`, already included via the core headers).
Run: `/opt/homebrew/bin/pio test -e native -f test_core` → still green (shared headers untouched here).

- [ ] **Step 6: Commit**

```bash
git add src/flight_ticker.ino
git commit -m "feat: render heading vectors, altitude color, emergency squawk on the radar"
```

---

## Task 5: App `Aircraft` + `parseAircraft` (Dart)

**Files:**
- Modify: `companion/lib/data/aircraft.dart`, `companion/lib/data/airplanes_client.dart`
- Test: `companion/test/airplanes_client_test.dart`

READ `companion/lib/data/aircraft.dart` (the `Aircraft` const constructor) and `parseAircraft`.

- [ ] **Step 1: Write the failing test**

Add to `companion/test/airplanes_client_test.dart` before the closing `}` of `main()`:
```dart
  test('parseAircraft extracts track and squawk', () {
    const body = '{"ac":[{"flight":"AB","t":"A320","lat":0.0,"lon":0.1,'
        '"alt_baro":10000,"gs":300,"track":275.4,"squawk":"7700"}]}';
    final a = parseAircraft(body, 0.0, 0.0).single;
    expect(a.track, closeTo(275.4, 0.1));
    expect(a.squawk, 7700);
    // Missing -> null.
    final b = parseAircraft('{"ac":[{"flight":"X","lat":0.0,"lon":0.1}]}', 0.0, 0.0).single;
    expect(b.track, isNull);
    expect(b.squawk, isNull);
  });
```

- [ ] **Step 2: Run to verify failure**

Run: `cd companion && flutter test test/airplanes_client_test.dart`
Expected: FAIL — `Aircraft` has no `track`/`squawk`.

- [ ] **Step 3: Add fields + extraction**

In `companion/lib/data/aircraft.dart`, add two **optional nullable** fields to `Aircraft` and the const constructor (NOT required, so existing `Aircraft(...)` constructions stay valid and default to null):
```dart
  final double? track; // true track degrees; null if missing
  final int? squawk;   // transponder code; null if missing
```
Add `this.track,` and `this.squawk,` (no `required`) to the const constructor. Existing call sites are unaffected.

In `companion/lib/data/airplanes_client.dart` `parseAircraft`, after computing `gsKt`, add:
```dart
    final double? track = (item['track'] is num) ? (item['track'] as num).toDouble() : null;
    final int? squawk = (item['squawk'] is String) ? int.tryParse(item['squawk'] as String) : null;
```
and pass `track: track, squawk: squawk` into the `Aircraft(...)` construction.

- [ ] **Step 4: Run to verify pass**

Run: `cd companion && flutter test`
Expected: all pass (update any other `Aircraft(...)` call in tests — e.g. in `ble_packet_test.dart`'s `_ac` helper and `aircraft_test.dart` — to include `track`/`squawk`; the analyzer/test failure will point to each).

- [ ] **Step 5: Commit**

```bash
git add companion/lib/data/aircraft.dart companion/lib/data/airplanes_client.dart companion/test/airplanes_client_test.dart
git commit -m "feat(companion): parse track + squawk into Aircraft"
```

---

## Task 6: App wire format v2 encoder (ble_packet.dart)

**Files:**
- Modify: `companion/lib/packet/ble_packet.dart`
- Test: `companion/test/ble_packet_test.dart`

READ `companion/lib/packet/ble_packet.dart` (constants, `encodePacket`, `_writeField`) and the `_ac` helper in `ble_packet_test.dart`.

- [ ] **Step 1: Write the failing test + update `_ac`**

In `companion/test/ble_packet_test.dart`, update the `_ac` helper to accept track/squawk and pass them to `Aircraft` (also satisfies Task 5's required fields):
```dart
Aircraft _ac({
  String cs = 'AAA', String ty = 'A320',
  double lat = 0, double lon = 0, int? alt = 1000, int? gs = 300, bool ground = false,
  double? track, int? squawk,
}) => Aircraft(callsign: cs, type: ty, lat: lat, lon: lon, altFt: alt, gsKt: gs,
    onGround: ground, track: track, squawk: squawk);
```
Add a v2 test:
```dart
  test('encodePacket v2 writes track and squawk with valid flags', () {
    final bytes = encodePacket(48.0, 11.0, [_ac(cs: 'DLH', track: 287, squawk: 7700)]);
    expect(bytes[2], 2);                 // version
    expect(bytes.length, 12 + 32);       // one 32-byte record
    final r = ByteData.sublistView(bytes, 12);
    expect(r.getInt16(28, Endian.little), 287);
    expect(r.getUint16(30, Endian.little), 7700);
    expect(r.getUint8(26) & bleFlagTrackValid, bleFlagTrackValid);
    expect(r.getUint8(26) & bleFlagSquawkValid, bleFlagSquawkValid);
    // null track/squawk -> flags clear.
    final b2 = encodePacket(0, 0, [_ac(cs: 'X')]);
    expect(ByteData.sublistView(b2, 12).getUint8(26) & bleFlagTrackValid, 0);
  });
```
NOTE: the existing `ble_packet_test` cases assert `bytes.length == 12 + 28` and offsets — UPDATE those to `12 + 32` (record is now 32 B); field offsets 0–27 are unchanged. The cap test (`count caps at 16`) becomes cap at 15 → assert `bytes[3] == 15` and `length == 12 + 15*32`.

- [ ] **Step 2: Run to verify failure**

Run: `cd companion && flutter test test/ble_packet_test.dart`
Expected: FAIL — `bleFlagTrackValid` undefined, record-size mismatch.

- [ ] **Step 3: Bump `ble_packet.dart` to v2**

In `companion/lib/packet/ble_packet.dart`:
- `const int bleVersion = 2;`
- `const int bleMaxAircraft = 15;`
- `const int bleRecordSize = 32;`
- add `const int bleFlagTrackValid = 0x08;` and `const int bleFlagSquawkValid = 0x10;`
- In `encodePacket`'s per-record loop, after writing `flags` + pad, append track + squawk and set the flags:
```dart
    var flags = 0;
    if (a.onGround) flags |= bleFlagGround;
    if (a.altFt != null) flags |= bleFlagAltValid;
    if (a.gsKt != null) flags |= bleFlagGsValid;
    if (a.track != null) flags |= bleFlagTrackValid;
    if (a.squawk != null) flags |= bleFlagSquawkValid;
    out[base + 26] = flags;
    out[base + 27] = 0; // pad
    bd.setInt16(base + 28, a.track?.round() ?? 0, Endian.little);
    bd.setUint16(base + 30, a.squawk ?? 0, Endian.little);
```
(Replace the existing flags/pad write with this block; `bleMaxPacket` recomputes from the constants.)

- [ ] **Step 4: Run to verify pass**

Run: `cd companion && flutter test`
Expected: all pass (15 + new, with updated offsets/cap).

- [ ] **Step 5: Commit**

```bash
git add companion/lib/packet/ble_packet.dart companion/test/ble_packet_test.dart
git commit -m "feat(companion): BLE wire v2 encoder — track + squawk, record 32B, cap 15"
```

---

## Task 7: `ble_send.py` v2 + on-device verification

**Files:**
- Modify: `scripts/ble_send.py`

- [ ] **Step 1: Update `ble_send.py` to v2**

READ `scripts/ble_send.py`. Update so each record is 32 bytes with track + squawk and version 2:
- Header version byte: change the `1` to `2` in the `struct.pack("<BBBB", 0x46, 0x52, 1, ...)` → `2`.
- Flag constants: add `FLAG_TRACK_VALID, FLAG_SQUAWK_VALID = 0x08, 0x10`.
- `_record`: append `struct.pack("<hH", track, squawk)` (int16 track, uint16 squawk) after the existing `<ffihBB` block, and add `track`/`squawk` params:
```python
def _record(cs, ty, lat, lon, alt_ft, gs_kt, flags, track=0, squawk=0) -> bytes:
    return (_field(cs, 8) + _field(ty, 4)
            + struct.pack("<ffihBB", lat, lon, alt_ft, gs_kt, flags, 0)
            + struct.pack("<hH", track, squawk))
```
- In the sample `aircraft` list, set the valid flags + values, and make ONE of them an emergency 7700 for testing, e.g.:
```python
("RYR4KP", "B738", 38.80, -9.28, 12000, 420, FLAG_ALT_VALID | FLAG_GS_VALID | FLAG_TRACK_VALID | FLAG_SQUAWK_VALID, 270, 1200),
("EMERG1", "A320", 38.72, -9.40, 35000, 450, FLAG_ALT_VALID | FLAG_GS_VALID | FLAG_TRACK_VALID | FLAG_SQUAWK_VALID, 90, 7700),
```
(adjust the `_packet`/loop to pass the extra tuple fields through to `_record`).

- [ ] **Step 2: Compile + flash**

Run: `/opt/homebrew/bin/pio run -e esp32-s3 -t upload`
Expected: `[SUCCESS]`, board flashes (the firmware already built in Task 4 Step 5).

- [ ] **Step 3: On-device — Wi-Fi path**

With Wi-Fi up, confirm on the radar: blips are colored by altitude (low warm / high cool), each shows a short heading line, the nearest has a white ring + label. If a real nearby aircraft squawks an emergency it would blink red — unlikely, so verify emergency via BLE next.

- [ ] **Step 4: On-device — emergency via BLE**

Set Wi-Fi down (bad SSID, as in prior tests), then run: `/tmp/ble_venv/bin/python scripts/ble_send.py` (or `pip install bleak`).
Expected: cyan **B**, blips with heading vectors + altitude colors, and the `EMERG1` aircraft (squawk 7700) **blinks red** with an `EMERGENCY 7700` banner. Restore Wi-Fi after.

- [ ] **Step 5: Commit**

```bash
git add scripts/ble_send.py
git commit -m "test: ble_send v2 packets with track/squawk + a 7700 emergency sample"
```

---

## Done criteria

- `pio test -e native -f test_core` and `cd companion && flutter test` green (incl. v2 round-trip, parse, and the three pure helpers).
- `pio run -e esp32-s3` compiles.
- On device (both Wi-Fi and BLE-fed): blips carry heading vectors, are colored by altitude band with the nearest ringed in white + labeled, and a 7500/7600/7700 aircraft blinks red with an `EMERGENCY <code>` banner.
