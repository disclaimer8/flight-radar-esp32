# Radar Range Presets + Out-of-Range Rim Dots Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Split the single `RADIUS_NM` into a fixed reception radius and a runtime display range; add touch zoom (3 presets), out-of-range rim dots, and NVS persistence.

**Architecture:** The API always polls the widest preset (100 km / 54 NM). A runtime `g_rangeIdx` selects the display range; `displayRangeKm()` replaces `rangeKm()`. `drawRadar` projects every aircraft with the display range (which clamps far ones onto the outer ring) and branches blip style on `isOnRim`: in-range keeps today's styling, beyond-range renders a small grey rim dot. Touch UP/DOWN cycles presets (clamped) and persists the index to NVS. The BLE wire format is untouched — rim plotting is a device-side render decision.

**Tech Stack:** C++ (Arduino/ESP32-S3), TFT_eSPI sprite, CST816S touch, ESP32 `Preferences` (NVS), PlatformIO native Unity tests.

**Spec:** `docs/superpowers/specs/2026-06-03-radar-range-rim-design.md`

---

## File structure

- `src/render_core.h` — add pure helpers `kRangePresets`/`kRangeCount`/`clampRangeIndex`/`isOnRim`/`queryRadiusNm` (Arduino-free, host-tested). **Task 1.**
- `test/test_core/test_main.cpp` — unit tests for the three new pure helpers. **Task 1.**
- `src/flight_ticker.ino` — range state + `displayRangeKm()`, wider poll cap + derived query radius (**Task 2**); rim rendering + range readout in `drawRadar` (**Task 3**); touch zoom + NVS load/save (**Task 4**). Arduino glue, verified by compile + on-device.
- `src/config.example.h` — note that `RADIUS_NM` no longer drives the poll. **Task 2.**

Tasks 2-4 touch only `.ino`/`config.example.h` glue, so they carry no host tests; each ends by compiling `pio run -e esp32-s3`. Task 1 is pure TDD.

---

### Task 1: Pure range helpers (render_core.h)

**Files:**
- Modify: `src/render_core.h` (append after `airlineCode`, before the final line)
- Test: `test/test_core/test_main.cpp`

- [ ] **Step 1: Write the failing tests**

Add these three test functions in `test/test_core/test_main.cpp` immediately before `void setUp(void) {}` (near line 483):

```cpp
void test_clamp_range_index(void) {
    TEST_ASSERT_EQUAL_INT(0, clampRangeIndex(0, -1, 3)); // clamp at low end
    TEST_ASSERT_EQUAL_INT(2, clampRangeIndex(2, +1, 3)); // clamp at high end
    TEST_ASSERT_EQUAL_INT(1, clampRangeIndex(0, +1, 3)); // step up
    TEST_ASSERT_EQUAL_INT(1, clampRangeIndex(2, -1, 3)); // step down
    TEST_ASSERT_EQUAL_INT(0, clampRangeIndex(1, -1, 3)); // middle down
}

void test_is_on_rim(void) {
    TEST_ASSERT_FALSE(isOnRim(10.0, 25.0));  // inside the range
    TEST_ASSERT_FALSE(isOnRim(25.0, 25.0));  // exactly on the boundary = in range
    TEST_ASSERT_TRUE(isOnRim(30.0, 25.0));   // beyond the range
}

void test_query_radius_nm(void) {
    TEST_ASSERT_EQUAL_INT(54, queryRadiusNm(100.0)); // widest preset
    TEST_ASSERT_EQUAL_INT(27, queryRadiusNm(50.0));  // sanity vs today's 27 NM
}
```

Register them in `main()` after `RUN_TEST(test_airline_code);` (near line 534):

```cpp
    RUN_TEST(test_clamp_range_index);
    RUN_TEST(test_is_on_rim);
    RUN_TEST(test_query_radius_nm);
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `pio test -e native -f test_core`
Expected: FAIL to compile — `clampRangeIndex`, `isOnRim`, `queryRadiusNm` not declared.

- [ ] **Step 3: Implement the helpers**

In `src/render_core.h`, append after the `airlineCode` function (after line 123, before the file ends). `<cmath>` is already included for `std::ceil`:

```cpp

// --- Display-range presets + radar zoom helpers ---

// Display-range presets in km, ascending (index 0 = nearest zoom, last = widest).
// The widest preset doubles as the fixed API reception radius.
inline constexpr double kRangePresets[] = {25.0, 50.0, 100.0};
inline constexpr int    kRangeCount = 3;

// Clamp idx+delta into [0, count-1]. Ladder semantics: no wrap at the ends.
inline int clampRangeIndex(int idx, int delta, int count) {
    int n = idx + delta;
    if (n < 0) n = 0;
    if (n > count - 1) n = count - 1;
    return n;
}

// True when an aircraft sits beyond the display range (-> draw it as a rim dot).
// Exactly on the boundary counts as in-range.
inline bool isOnRim(double distKm, double displayRangeKm) {
    return distKm > displayRangeKm;
}

// API query radius in nautical miles for a reception radius given in km, rounded up
// (1 NM = 1.852 km). Used to build the airplanes.live poll URL.
inline int queryRadiusNm(double maxPresetKm) {
    return (int)std::ceil(maxPresetKm / 1.852);
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `pio test -e native -f test_core`
Expected: PASS — all cases succeed (existing count + 3 new = 50).

- [ ] **Step 5: Commit**

```bash
git add src/render_core.h test/test_core/test_main.cpp
git commit -m "feat(render): range presets + clampRangeIndex/isOnRim/queryRadiusNm helpers"
```

---

### Task 2: Range state + derived poll radius + wider cap (flight_ticker.ino)

**Files:**
- Modify: `src/flight_ticker.ino` (constants near line 19; state near line 43; `rangeKm()` line 99; `pollApi` lines 117-134)
- Modify: `src/config.example.h` (RADIUS_NM comment)

This task is Arduino glue; no host test. Verify by compiling.

- [ ] **Step 1: Add the radar-plot cap constant**

In `src/flight_ticker.ino`, after the `CX/CY/MAXR` line (line 19), add:

```cpp
// Wi-Fi parse cap: keep more than the display cap so distant aircraft survive to
// be drawn as rim dots (the nearest 24 by distance). The detail carousel pages all.
static const int RADAR_PLOT_CAP = 24;
```

- [ ] **Step 2: Add the runtime range index**

In `src/flight_ticker.ino`, in the view-state block (after `size_t g_idx = 0;`, line 43), add:

```cpp
int g_rangeIdx = 1;  // index into kRangePresets; default 50 km. Restored from NVS in setup().
```

- [ ] **Step 3: Replace `rangeKm()` with `displayRangeKm()`**

In `src/flight_ticker.ino`, replace the `rangeKm()` helper (line 99):

```cpp
static double rangeKm() { return RADIUS_NM * 1.852; }
```

with:

```cpp
// Current display range (outer ring) in km, selected by touch zoom. Replaces the
// old fixed rangeKm(); the API reception radius is separate (see pollApi()).
static double displayRangeKm() { return kRangePresets[g_rangeIdx]; }
```

- [ ] **Step 4: Derive the poll radius from the widest preset + raise the cap**

In `src/flight_ticker.ino` `pollApi()`, change the URL radius (line 118-120) from `(int)RADIUS_NM` to the derived query radius:

```cpp
    std::snprintf(url, sizeof(url),
        "https://api.airplanes.live/v2/point/%.4f/%.4f/%d",
        (double)MY_LAT, (double)MY_LON, queryRadiusNm(kRangePresets[kRangeCount - 1]));
```

And in the same function change the parse cap (line 134) from `MAX_AIRCRAFT` to `RADAR_PLOT_CAP`:

```cpp
        g_cache = parseNearest(std::string(payload.c_str()), MY_LAT, MY_LON, RADAR_PLOT_CAP, HIDE_GROUND_AIRCRAFT);
```

- [ ] **Step 5: Note the config change**

In `src/config.example.h`, update the `RADIUS_NM` comment so it no longer claims to drive the poll. Change its line to:

```cpp
#define RADIUS_NM         27      // (legacy) no longer used: the poll radius is now
                                  // derived from the widest range preset (100 km / 54 NM)
```

(The firmware no longer references `RADIUS_NM`; the gitignored `src/config.h` keeps its own copy and needs no edit.)

- [ ] **Step 6: Update the `drawRadar` call site so the file still compiles**

`rangeKm()` no longer exists, but `drawRadar()` still calls it (line 173). Replace that single call with `displayRangeKm()` (Task 3 rewrites this loop fully, but this keeps Task 2 self-contained and compilable):

```cpp
        ScreenPoint p = polarToXY(b, ac.distKm, displayRangeKm(), CX, CY, MAXR);
```

- [ ] **Step 7: Compile**

Run: `pio run -e esp32-s3`
Expected: SUCCESS — links and builds.

- [ ] **Step 8: Commit**

```bash
git add src/flight_ticker.ino src/config.example.h
git commit -m "feat(radar): display range state + fixed wide poll radius + 24-cap"
```

---

### Task 3: Rim dots + range readout (drawRadar)

**Files:**
- Modify: `src/flight_ticker.ino` `drawRadar()` (blip loop lines 166-195; add readout before `pushSprite` line 227)

Arduino glue; no host test. Verify by compile + on-device.

- [ ] **Step 1: Replace the blip loop with range-aware rendering**

In `src/flight_ticker.ino` `drawRadar()`, replace the entire blip loop block (from the comment `// blips: colored by altitude...` through the closing brace of the `for` loop, lines 166-195) with:

```cpp
    // blips: in-range keep altitude color + heading vector (nearest gets a white
    // ring + label); aircraft beyond the display range render as small grey rim
    // dots at their bearing. Emergencies are detected regardless of range.
    bool blinkOn = (millis() / 500) % 2 == 0;
    bool anyEmergency = false;
    int  emergencyCode = 0;
    double dr = displayRangeKm();
    for (size_t i = 0; i < g_cache.size(); i++) {
        const Aircraft& ac = g_cache[i];
        double b = bearingDeg(g_centerLat, g_centerLon, ac.lat, ac.lon);
        ScreenPoint p = polarToXY(b, ac.distKm, dr, CX, CY, MAXR);

        bool emerg = isEmergencySquawk(ac.squawk);
        if (emerg) { anyEmergency = true; emergencyCode = ac.squawk; }

        if (isOnRim(ac.distKm, dr)) {
            uint16_t rc = (emerg && blinkOn) ? TFT_RED : TFT_DARKGREY;
            fb.fillCircle(p.x, p.y, 1, rc);
            continue;
        }

        uint16_t color = kAltColors[altBand(ac.altFt, ac.onGround)];
        if (emerg) color = blinkOn ? TFT_RED : TFT_DARKGREY;

        if (!std::isnan(ac.track)) {
            ScreenPoint e = vectorEnd(p, ac.track, 10.0);
            fb.drawLine(p.x, p.y, e.x, e.y, color);
        }

        if (i == 0) {
            fb.fillCircle(p.x, p.y, 4, color);
            fb.drawCircle(p.x, p.y, 6, TFT_WHITE); // nearest ring (in-range only)
            std::string cs = ac.callsign.empty() ? "------" : ac.callsign;
            fb.setTextDatum(TL_DATUM);
            fb.setTextColor(TFT_WHITE, TFT_BLACK);
            fb.drawString(cs.c_str(), p.x + 8, p.y - 4, 2);
        } else {
            fb.fillCircle(p.x, p.y, 2, color);
        }
    }
```

(The nearest-highlight `if (i == 0)` is now only reached when the nearest aircraft is in range — when it is on the rim, the `continue` above draws it as a plain rim dot, matching the spec.)

- [ ] **Step 2: Add the range readout**

In `src/flight_ticker.ino` `drawRadar()`, just before `fb.pushSprite(0, 0);` (line 227), add the top-left range label:

```cpp
    // Range readout (top-left): names the outer-ring distance / current zoom.
    char rbuf[8];
    std::snprintf(rbuf, sizeof(rbuf), "%dkm", (int)dr);
    fb.setTextDatum(TL_DATUM);
    fb.setTextColor(TFT_DARKGREEN, TFT_BLACK);
    fb.drawString(rbuf, 4, 4, 2);

```

- [ ] **Step 3: Compile**

Run: `pio run -e esp32-s3`
Expected: SUCCESS.

- [ ] **Step 4: Commit**

```bash
git add src/flight_ticker.ino
git commit -m "feat(radar): out-of-range rim dots + range readout in drawRadar"
```

---

### Task 4: Touch zoom + NVS persistence (handleTouch, setup)

**Files:**
- Modify: `src/flight_ticker.ino` (include + `Preferences` near top; `saveRangeIdx()` helper; `handleTouch` RADAR branch lines 303-304; `setup()` load near line 323)

Arduino glue; no host test. Verify by compile + on-device.

- [ ] **Step 1: Include Preferences**

In `src/flight_ticker.ino`, after `#include <map>` (line 13), add:

```cpp
#include <Preferences.h>
```

- [ ] **Step 2: Add the NVS save helper**

In `src/flight_ticker.ino`, after the `g_rangeIdx` declaration added in Task 2, add a helper that persists it (place it after the `lookupRoute` function, near line 97, so `g_rangeIdx` is already in scope):

```cpp
// Persist the selected range index to NVS so the zoom survives reboot. Called only
// on a user-driven change (not per frame), so flash wear is negligible.
void saveRangeIdx() {
    Preferences prefs;
    prefs.begin("radar", false);   // read-write
    prefs.putInt("rangeIdx", g_rangeIdx);
    prefs.end();
}
```

- [ ] **Step 3: Map UP/DOWN to zoom in the RADAR branch**

In `src/flight_ticker.ino` `handleTouch()`, replace the RADAR branch (lines 303-305):

```cpp
    if (g_view == RADAR) {
        if (g == TG_CLICK) { g_view = DETAIL; g_idx = 0; }
    } else { // DETAIL
```

with:

```cpp
    if (g_view == RADAR) {
        if (g == TG_CLICK) {
            g_view = DETAIL; g_idx = 0;
        } else if (g == TG_UP) {               // zoom in (smaller range)
            int n = clampRangeIndex(g_rangeIdx, -1, kRangeCount);
            if (n != g_rangeIdx) { g_rangeIdx = n; saveRangeIdx(); }
        } else if (g == TG_DOWN) {              // zoom out (larger range)
            int n = clampRangeIndex(g_rangeIdx, +1, kRangeCount);
            if (n != g_rangeIdx) { g_rangeIdx = n; saveRangeIdx(); }
        }
    } else { // DETAIL
```

- [ ] **Step 4: Load the saved index in setup()**

In `src/flight_ticker.ino` `setup()`, after `touch.begin();` (line 323), add the NVS load (validated into range with `clampRangeIndex` against bounds):

```cpp
    // Restore the saved display range (default 50 km = index 1), clamped valid.
    {
        Preferences prefs;
        prefs.begin("radar", true);    // read-only
        int saved = prefs.getInt("rangeIdx", 1);
        prefs.end();
        if (saved < 0) saved = 0;
        if (saved > kRangeCount - 1) saved = kRangeCount - 1;
        g_rangeIdx = saved;
    }
```

- [ ] **Step 5: Compile**

Run: `pio run -e esp32-s3`
Expected: SUCCESS.

- [ ] **Step 6: Commit**

```bash
git add src/flight_ticker.ino
git commit -m "feat(radar): touch UP/DOWN range zoom + NVS persistence"
```

---

### Task 5: Full test run + on-device acceptance

**Files:** none (verification only)

- [ ] **Step 1: Run the full native suite**

Run: `pio test -e native -f test_core`
Expected: PASS — all cases (existing + 3 new helpers) succeed.

- [ ] **Step 2: Build firmware**

Run: `pio run -e esp32-s3`
Expected: SUCCESS.

- [ ] **Step 3: Flash and verify on device** (manual, requires hardware)

Run: `pio run -e esp32-s3 -t upload`

Acceptance checklist:
- Swipe up / down on the radar cycles 25 → 50 → 100 km; the top-left readout updates; it clamps at both ends (no wrap).
- At 25 km, aircraft between 25 and 100 km appear as small grey dots on the outer ring at their correct bearing; at 100 km the rim is empty.
- Power-cycle the device: it boots at the last-selected range (NVS restore).
- A tap still opens the detail carousel; swipe left/right pages; tap/down returns.
- (If observable) an emergency-squawk aircraft beyond the display range still fires the center EMERGENCY banner.

---

## After implementation

Use superpowers:finishing-a-development-branch to verify tests, then merge + push per the established cadence. A note-only follow-up remains (carried from sub-project 1): the docs (README/ARCHITECTURE/HARDWARE/CLAUDE.md) still describe the v2 wire / cap 15 / single `RADIUS_NM` and don't mention reg/route, rim dots, or range presets — a future docs-refresh pass.
