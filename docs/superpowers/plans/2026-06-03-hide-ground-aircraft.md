# Hide Ground Aircraft Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Hide on-ground aircraft from the radar and the detail carousel (both Wi-Fi and BLE data paths), toggleable via config, by filtering them at the parse/selection stage.

**Architecture:** Add a `hideGround` parameter (default `false` for backward-compat) to the three pure selection functions — `parseNearest` (firmware Wi-Fi), `parseBlePacket` (firmware BLE), `parseAircraft` (app). When true, skip `onGround` aircraft BEFORE the nearest-N sort+cap, so airborne traffic fills the slots and ground aircraft never reach the render path. Callers supply the toggle from config (`HIDE_GROUND_AIRCRAFT` in firmware `config.h`; `kHideGroundAircraft` const in the app).

**Tech Stack:** C++ (PlatformIO native Unity tests), Dart (flutter_test).

**Why default `false`:** existing native + Dart tests include ground aircraft and assert they're present; a default-true would break them. The product default (hide) comes from the callers passing the config value.

---

## Task 1: Firmware Wi-Fi path — `parseNearest` filter + config

**Files:**
- Modify: `src/config.h`, `src/config.example.h`
- Modify: `src/flight_core.h`
- Modify: `src/flight_ticker.ino`
- Test: `test/test_core/test_main.cpp`

First READ `src/flight_core.h` to find the exact `parseNearest` signature and where it builds/sorts/caps the aircraft list, and READ the `parseNearest(...)` call in `src/flight_ticker.ino`.

- [ ] **Step 1: Write the failing test**

In `test/test_core/test_main.cpp`, add this test before `void setUp` (it builds an airplanes.live-shaped JSON with a NEAR ground aircraft and two farther airborne ones):

```cpp
void test_parse_nearest_hides_ground(void) {
    // center 0,0. GND is nearest (0.1) but on the ground; A1 (0.2) and A2 (0.3) airborne.
    const char* json =
        "{\"ac\":["
        "{\"flight\":\"GND1\",\"t\":\"B772\",\"lat\":0.0,\"lon\":0.1,\"alt_baro\":\"ground\",\"gs\":3},"
        "{\"flight\":\"AIR1\",\"t\":\"A320\",\"lat\":0.0,\"lon\":0.2,\"alt_baro\":10000,\"gs\":300},"
        "{\"flight\":\"AIR2\",\"t\":\"A320\",\"lat\":0.0,\"lon\":0.3,\"alt_baro\":20000,\"gs\":400}"
        "]}";
    // hideGround = true: GND excluded, the two nearest AIRBORNE fill the slots.
    auto kept = parseNearest(json, 0.0, 0.0, 2, true);
    TEST_ASSERT_EQUAL_UINT32(2, kept.size());
    TEST_ASSERT_EQUAL_STRING("AIR1", kept[0].callsign.c_str());
    TEST_ASSERT_EQUAL_STRING("AIR2", kept[1].callsign.c_str());
    // hideGround = false: nearest 2 include the ground aircraft (current behavior).
    auto all = parseNearest(json, 0.0, 0.0, 2, false);
    TEST_ASSERT_EQUAL_UINT32(2, all.size());
    TEST_ASSERT_EQUAL_STRING("GND1", all[0].callsign.c_str());
    TEST_ASSERT_EQUAL_STRING("AIR1", all[1].callsign.c_str());
}
```

Register it in `main()` after the last existing `RUN_TEST`:
```cpp
    RUN_TEST(test_parse_nearest_hides_ground);
```

- [ ] **Step 2: Run tests to verify failure**

Run: `/opt/homebrew/bin/pio test -e native -f test_core`
Expected: FAIL — `parseNearest` does not accept a 5th argument yet.

- [ ] **Step 3: Add the `hideGround` parameter + filter to `parseNearest`**

In `src/flight_core.h`, add a trailing parameter `bool hideGround = false` to `parseNearest`'s signature. In the loop that builds each `Aircraft` from a JSON entry, AFTER `onGround` is determined and BEFORE the aircraft is added to the working vector, add the skip:

```cpp
        if (hideGround && ac.onGround) continue;
```
(Use the real local variable name for the aircraft from the existing code — read it first. The skip must be before the `push_back`/insert into the list that gets sorted and capped, so a skipped ground aircraft frees its nearest-N slot.)

- [ ] **Step 4: Run tests to verify pass**

Run: `/opt/homebrew/bin/pio test -e native -f test_core`
Expected: PASS — the new test plus all existing tests (existing ones call `parseNearest` without the 5th arg, defaulting `hideGround=false`, so they are unaffected).

- [ ] **Step 5: Add the config define**

In `src/config.example.h`, after the `BLE_FRESHNESS_MS` line, add:
```cpp
#define HIDE_GROUND_AIRCRAFT  1   // 1 = hide on-ground aircraft from radar + list
```
Add the same line to `src/config.h` (the live, gitignored config).

- [ ] **Step 6: Pass the config flag at the Wi-Fi call site**

In `src/flight_ticker.ino`, find the `parseNearest(...)` call in `pollApi()` and add the flag as the final argument: `parseNearest(std::string(payload.c_str()), MY_LAT, MY_LON, MAX_AIRCRAFT, HIDE_GROUND_AIRCRAFT)` (match the real argument list; just append `, HIDE_GROUND_AIRCRAFT`).

- [ ] **Step 7: Commit**

```bash
git add src/flight_core.h src/config.example.h test/test_core/test_main.cpp src/flight_ticker.ino
git commit -m "feat: hide on-ground aircraft on the Wi-Fi path (parseNearest hideGround + config)"
```
(`src/config.h` is gitignored — edit it in place but it won't be committed.)

---

## Task 2: Firmware BLE path — `parseBlePacket` filter

**Files:**
- Modify: `src/ble_core.h`
- Modify: `src/flight_ticker.ino`
- Test: `test/test_core/test_main.cpp`

READ `src/ble_core.h` for the exact `parseBlePacket` signature and its decode loop, and the `parseBlePacket(...)` call in `src/flight_ticker.ino`. The existing BLE test helpers (`bleHeader`, `bleAddRecord`, flag constants) are already in `test_main.cpp`.

- [ ] **Step 1: Write the failing test**

Add before `void setUp` in `test/test_core/test_main.cpp`:

```cpp
void test_ble_hides_ground(void) {
    // center 0,0. GND nearest (0.1, ground); A1 (0.2) and A2 (0.3) airborne.
    std::vector<uint8_t> v = bleHeader(3, 0.0f, 0.0f);
    bleAddRecord(v, "GND1", "B772", 0.0f, 0.1f, 0, 5, BLE_FLAG_GROUND);
    bleAddRecord(v, "AIR1", "A320", 0.0f, 0.2f, 10000, 300, BLE_FLAG_ALT_VALID | BLE_FLAG_GS_VALID);
    bleAddRecord(v, "AIR2", "A320", 0.0f, 0.3f, 20000, 400, BLE_FLAG_ALT_VALID | BLE_FLAG_GS_VALID);
    // hideGround = true: GND excluded; two nearest airborne fill the slots.
    BlePacket hid = parseBlePacket(v.data(), v.size(), 2, true);
    TEST_ASSERT_TRUE(hid.ok);
    TEST_ASSERT_EQUAL_UINT32(2, hid.aircraft.size());
    TEST_ASSERT_EQUAL_STRING("AIR1", hid.aircraft[0].callsign.c_str());
    TEST_ASSERT_EQUAL_STRING("AIR2", hid.aircraft[1].callsign.c_str());
    // hideGround = false: nearest 2 include the ground aircraft.
    BlePacket all = parseBlePacket(v.data(), v.size(), 2, false);
    TEST_ASSERT_TRUE(all.ok);
    TEST_ASSERT_EQUAL_STRING("GND1", all.aircraft[0].callsign.c_str());
    TEST_ASSERT_EQUAL_STRING("AIR1", all.aircraft[1].callsign.c_str());
}
```

Register in `main()`:
```cpp
    RUN_TEST(test_ble_hides_ground);
```

- [ ] **Step 2: Run tests to verify failure**

Run: `/opt/homebrew/bin/pio test -e native -f test_core`
Expected: FAIL — `parseBlePacket` does not accept a 4th argument yet.

- [ ] **Step 3: Add `hideGround` + filter to `parseBlePacket`**

In `src/ble_core.h`, add a trailing parameter `bool hideGround = false` to `parseBlePacket`. In the record-decode loop, after the `Aircraft`'s `onGround` is set and BEFORE it is pushed into the vector that gets sorted and capped, add:
```cpp
        if (hideGround && ac.onGround) continue;
```
(Match the real local aircraft variable name from the existing loop. The length validation on the raw buffer is unchanged; skipping a decoded record just yields fewer entries in the output vector.)

- [ ] **Step 4: Run tests to verify pass**

Run: `/opt/homebrew/bin/pio test -e native -f test_core`
Expected: PASS — new test + all existing (existing calls default `hideGround=false`).

- [ ] **Step 5: Pass the config flag at the BLE call site**

In `src/flight_ticker.ino`, find the `parseBlePacket(g_bleBuf, g_bleLen, MAX_AIRCRAFT)` call (in `loop()`'s packet handler) and append the flag: `parseBlePacket(g_bleBuf, g_bleLen, MAX_AIRCRAFT, HIDE_GROUND_AIRCRAFT)`.

- [ ] **Step 6: Commit**

```bash
git add src/ble_core.h test/test_core/test_main.cpp src/flight_ticker.ino
git commit -m "feat: hide on-ground aircraft on the BLE path (parseBlePacket hideGround)"
```

---

## Task 3: Companion app — `parseAircraft` filter

**Files:**
- Modify: `companion/lib/data/airplanes_client.dart`
- Test: `companion/test/airplanes_client_test.dart`

READ `companion/lib/data/airplanes_client.dart` for the exact `parseAircraft` signature, its build/sort/trim, and `fetchNearby`.

- [ ] **Step 1: Write the failing test**

Add to `companion/test/airplanes_client_test.dart`, before the closing `}` of `main()`:

```dart
  test('parseAircraft hides on-ground aircraft when hideGround is true', () {
    const body = '''
{"ac":[
  {"flight":"GND1","t":"B772","lat":0.0,"lon":0.1,"alt_baro":"ground","gs":3},
  {"flight":"AIR1","t":"A320","lat":0.0,"lon":0.2,"alt_baro":10000,"gs":300},
  {"flight":"AIR2","t":"A320","lat":0.0,"lon":0.3,"alt_baro":20000,"gs":400}
]}
''';
    // hideGround true: GND (nearest) excluded; two nearest airborne kept.
    final hidden = parseAircraft(body, 0.0, 0.0, hideGround: true);
    expect(hidden.map((a) => a.callsign), ['AIR1', 'AIR2']);
    // hideGround false (default): nearest-first incl. the ground aircraft.
    final all = parseAircraft(body, 0.0, 0.0);
    expect(all.first.callsign, 'GND1');
  });
```

- [ ] **Step 2: Run test to verify failure**

Run: `cd companion && flutter test test/airplanes_client_test.dart`
Expected: FAIL — `parseAircraft` has no `hideGround` named parameter.

- [ ] **Step 3: Add the const, the parameter, and the filter**

In `companion/lib/data/airplanes_client.dart`:
- Add a top-level const (near the imports): `const bool kHideGroundAircraft = true;`
- Change `parseAircraft`'s signature to add a trailing named param: `List<Aircraft> parseAircraft(String body, double centerLat, double centerLon, {bool hideGround = false})`.
- In the loop that builds each `Aircraft`, after `onGround` is computed and BEFORE adding it to the list, add: `if (hideGround && onGround) continue;` (use the real local variable for the ground flag).
- In `AirplanesClient.fetchNearby`, pass the const to the parse call: `return parseAircraft(resp.body, lat, lon, hideGround: kHideGroundAircraft);`

- [ ] **Step 4: Run tests to verify pass**

Run: `cd companion && flutter test test/airplanes_client_test.dart`
Expected: PASS. Then the whole suite: `cd companion && flutter test` → all green (existing `parseAircraft` tests call without `hideGround`, defaulting false, so the existing ground-handling test is unaffected).

- [ ] **Step 5: Commit**

```bash
git add companion/lib/data/airplanes_client.dart companion/test/airplanes_client_test.dart
git commit -m "feat(companion): hide on-ground aircraft from the BLE packet (parseAircraft hideGround)"
```

---

## Done criteria

- `pio test -e native -f test_core` and `cd companion && flutter test` both green, including the new ground-filter cases.
- With default config (`HIDE_GROUND_AIRCRAFT 1`, `kHideGroundAircraft true`), on-ground aircraft appear neither as radar blips nor in the detail carousel, on both Wi-Fi and BLE paths (a ground aircraft nearer than airborne ones does not consume a nearest-N slot).
- Setting `HIDE_GROUND_AIRCRAFT 0` restores ground aircraft on the Wi-Fi path.

## Optional on-device check (not required for the plan to be "done")

Flash and confirm on the device (Wi-Fi up): aircraft known to be parked/taxiing no longer appear. Build gate without a device: `pio run -e esp32-s3` compiles with the new call sites.
