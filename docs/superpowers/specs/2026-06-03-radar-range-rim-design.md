# Radar UX — Out-of-Range Rim Dots + Range Presets — Design

> **Status:** approved design. Sub-project 2 of the 4-10 feature batch (features
> #4 rim dots + #7 range presets). Splits the single `RADIUS_NM` into a fixed
> reception radius and a runtime display range; adds touch zoom + rim plotting.

## Purpose

Today `RADIUS_NM` (27 NM ≈ 50 km) is **both** the API query radius and the display
range, and `polarToXY` clamps distance to that range — so aircraft "beyond the
radar" do not exist. This sub-project separates the two concepts:

- **Reception radius** — fixed at the largest preset; the API always polls this wide.
- **Display range** — a runtime zoom the user cycles by touch; the outer ring.

Aircraft farther than the display range but within reception become small dots on
the **rim** at their bearing (a direction hint to off-screen traffic). The selected
range survives reboot.

This delivers feature #4 (rim dots) and feature #7 (range presets) together — they
are inseparable, because rim dots only have meaning once reception > display range.

## Decisions (from brainstorm)

- **Range model:** fixed reception radius = max preset; zoom changes display only,
  with **no re-poll** (we already hold the wide data). Rim works on both Wi-Fi and BLE.
- **Presets:** `25 / 50 / 100 km`, default start = `50 km` (≈ today's behavior).
- **Cap:** raise the Wi-Fi parse cap to **24** so distant aircraft survive for the
  rim; the detail carousel pages all of them. `MAX_AIRCRAFT` (10) stays the BLE wire
  cap (the phone caps its own send).
- **Gesture:** `TG_UP` = zoom in (smaller range), `TG_DOWN` = zoom out (larger
  range), clamped at the ends (no wrap). `TG_CLICK` → detail (unchanged).
- **Persistence:** store the range index in NVS (Preferences), restore on boot.

## Range model

- Preset ladder (km, ascending): `kRangePresets = {25.0, 50.0, 100.0}`,
  `kRangeCount = 3`. Index 0 = nearest zoom (25 km), index 2 = widest (100 km).
- Runtime state: `int g_rangeIdx` (default 1 = 50 km).
- `displayRangeKm()` returns `kRangePresets[g_rangeIdx]` and **replaces** the
  current `rangeKm()` everywhere it is used (radar projection, detail bearing math
  is unaffected — it uses lat/lon, not range).
- **Reception radius is fixed** at the max preset (100 km). The API URL uses
  `queryRadiusNm(kRangePresets[kRangeCount-1])` = `ceil(100 / 1.852)` = **54 NM**
  (well under the 250 NM API cap; poll cadence unchanged at 15 s, ≤ 1/s).
- Because reception is fixed and ≥ every display range, a zoom change needs **no
  new poll** — the wide `g_cache` already contains the rim aircraft; only the
  projection and per-blip styling change.

## Parse cap

- Wi-Fi: `pollApi` calls `parseNearest(payload, MY_LAT, MY_LON, RADAR_PLOT_CAP,
  HIDE_GROUND_AIRCRAFT)` with `RADAR_PLOT_CAP = 24` (was `MAX_AIRCRAFT` = 10).
  `parseNearest` keeps the nearest N by distance; raising N to 24 means the far
  aircraft that belong on the rim are not truncated before the radar can plot them.
- The detail carousel pages the full `g_cache` (already does), so it now pages up
  to 24 on Wi-Fi. Page-dot rendering already shrinks spacing to fit, so no layout
  change is needed.
- `MAX_AIRCRAFT` (10) is unchanged and remains: the radar `MAX_AIRCRAFT` display
  doc meaning is superseded by `RADAR_PLOT_CAP` on Wi-Fi; on BLE the cache size is
  whatever the phone sent (≤ 10, the wire cap). The BLE wire format is untouched.

## Blip rendering (`drawRadar`)

Projection is unified: every aircraft is projected with
`polarToXY(bearing, ac.distKm, displayRangeKm(), CX, CY, MAXR)`, which already
clamps `distKm > displayRangeKm()` onto the outer ring (radius `MAXR`). Style then
branches on `isOnRim(ac.distKm, displayRangeKm())` (= `ac.distKm > displayRangeKm()`):

- **In-range** (`!isOnRim`): unchanged behavior — altitude-band color, heading
  vector, and for the nearest in-range aircraft a white ring + callsign label.
- **On rim** (`isOnRim`): a small dim dot at the clamped rim position — `fillCircle(p.x, p.y, 1, TFT_DARKGREY)`, **no** heading vector, **no** label,
  **no** nearest ring. If the aircraft is an emergency squawk, the rim dot is red
  and blinks (reuse `blinkOn`), but it stays size 1.
- **Nearest-highlight guard:** the current code unconditionally gives index 0 the
  white ring + label. With zoom, index 0 (nearest overall) may itself be on the
  rim. The white-ring/label treatment must apply only when index 0 is **in range**;
  if the nearest aircraft is on the rim, it renders as a plain rim dot. Concretely,
  the nearest-highlight branch is gated by `!isOnRim(...)`.
- **Emergency banner:** `anyEmergency` / `emergencyCode` detection stays global and
  range-independent — an emergency aircraft beyond the display range still triggers
  the center `EMERGENCY <code>` banner (safety: never hide an emergency by zoom).

### Range readout

Draw the current display range as small dim text so the zoom level is legible:
`drawString("<km>km", ...)` at the **top-left** (e.g. datum `TL_DATUM`, position
`(4, 4)`, font 2, color `TFT_DARKGREEN`). Built from `(long)displayRangeKm()` →
`"25km"` / `"50km"` / `"100km"`. The outer ring already visually encodes the range;
this label names it.

## Touch (`handleTouch`, RADAR branch)

In the `g_view == RADAR` branch, alongside the existing `TG_CLICK → DETAIL`:

- `TG_UP`: `g_rangeIdx = clampRangeIndex(g_rangeIdx, -1, kRangeCount)` (zoom in).
- `TG_DOWN`: `g_rangeIdx = clampRangeIndex(g_rangeIdx, +1, kRangeCount)` (zoom out).
- On any change (new index != old), persist to NVS immediately (see below).

Clamp (not wrap): at index 0, `TG_UP` is a no-op; at index 2, `TG_DOWN` is a no-op.
The DETAIL branch is unchanged (its `TG_DOWN` = return to radar is a different view,
no conflict).

## NVS persistence (Preferences)

- Namespace `"radar"`, key `"rangeIdx"` (a `uchar`/`int`).
- `setup()`: open Preferences read-only, read `rangeIdx` with default `1`, validate
  into `[0, kRangeCount-1]` (clamp out-of-range stored values), assign `g_rangeIdx`.
- On a touch-driven change: open Preferences read-write, `putInt`/`putUChar`, close.
  Writes only happen on user action (not per frame), so flash wear is negligible.
- This is Arduino glue (uses the ESP32 `Preferences` library) — verified on device,
  not host-unit-tested. The pure index logic (`clampRangeIndex`) is unit-tested.

## Pure helpers (host-tested, `render_core.h`)

Add alongside the existing pure helpers:

```cpp
// Display-range presets in km, ascending (index 0 = nearest zoom).
inline constexpr double kRangePresets[] = {25.0, 50.0, 100.0};
inline constexpr int    kRangeCount = 3;

// Clamp idx+delta into [0, count-1] (ladder semantics: no wrap at the ends).
inline int clampRangeIndex(int idx, int delta, int count) {
    int n = idx + delta;
    if (n < 0) n = 0;
    if (n > count - 1) n = count - 1;
    return n;
}

// True when an aircraft sits beyond the display range (→ rim dot).
inline bool isOnRim(double distKm, double displayRangeKm) {
    return distKm > displayRangeKm;
}

// API query radius (nautical miles) for a reception radius given in km, rounded up.
inline int queryRadiusNm(double maxPresetKm) {
    return (int)std::ceil(maxPresetKm / 1.852);
}
```

(`<cmath>` is already included for `std::ceil`.)

## Testing (TDD, host)

Pure unit tests (Unity, native env):
- `clampRangeIndex`: up from index 0 stays 0; down from `count-1` stays `count-1`;
  middle moves by delta; arbitrary count.
- `isOnRim`: inside range → false; exactly on the boundary (`distKm == range`) →
  false; beyond → true.
- `queryRadiusNm(100.0)` → 54; `queryRadiusNm(50.0)` → 27 (sanity vs today).

Glue verified on device, not unit-tested: NVS load/save, touch gesture mapping,
rim-dot drawing, range readout.

On-device acceptance:
- Swipe up/down on the radar cycles 25→50→100 km; the outer-ring meaning and the
  top-left readout both update; clamps at the ends.
- At 25 km, aircraft between 25 and 100 km appear as small grey dots on the rim at
  their correct bearing; at 100 km the rim is empty (nothing beyond reception).
- Reboot restores the last-selected range from NVS.
- An emergency-squawk aircraft beyond the display range still fires the EMERGENCY
  banner; its rim dot is red.

## Out of scope

- Smooth/animated zoom (discrete ladder steps only).
- Re-scaling the 3 range rings or labeling each ring (outer ring = display range;
  inner rings stay at 1/3, 2/3 visually).
- Any BLE wire change — rim plotting is purely a device-side render decision over
  whatever aircraft are already in `g_cache`.
- Per-aircraft "off-screen distance" readout on the rim (just a direction dot).

## Done criteria

- `pio test -e native -f test_core` green (incl. the three new pure helpers); the
  existing cases still pass.
- `pio run -e esp32-s3` compiles.
- On device: touch zoom cycles the presets with a visible range readout, distant
  aircraft render as rim dots at the right bearing, the selection persists across
  reboot, and emergencies remain visible at any zoom.
