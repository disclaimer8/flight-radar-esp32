# Phone Enrichment — Photos + Route Viewer (#9) + Emergency/Military Push (#10) — Design

> **Status:** approved design. Sub-project 3 of the 4-10 feature batch. Two
> companion-app (Flutter) features sharing one model/parser change and one
> feeder→UI data path. No firmware / BLE wire change.

## Purpose

The companion app is today a headless feeder with a minimal status UI (Device /
GPS / count + Start/Stop). This sub-project makes the phone useful in its own
right:

- **#9 — Photos + route viewer:** the home screen becomes a live list of the
  nearby aircraft the feeder is sending, each card showing callsign, type,
  distance, route (origin → dest), registration, and an aircraft photo.
- **#10 — Emergency/Military push:** when an emergency-squawk or military aircraft
  appears in a feed cycle, the app fires a local notification — even when feeding
  in the background.

Both features are app-internal; the BLE wire format and firmware are unchanged.

## Decisions (from brainstorm)

- **Data source for the viewer:** the feeder's per-cycle aircraft list is surfaced
  to the UI (not a separate foreground poll). On Android the list crosses the
  foreground-service isolate via `sendDataToMain`; on iOS the engine runs in the
  main isolate so the list is in-process. The viewer is empty when the feeder is
  stopped (accepted).
- **UI placement:** the home screen becomes the list (compact status + Start/Stop
  header above a scrollable card list).
- **Push categories:** emergency **and** military, both always on (no toggle).
- **Dependency:** add `flutter_local_notifications` (a new iOS Pod) for the alerts.
- **Scope:** one spec covering both features.

## Verified data sources

- **airplanes.live `/v2/point`** already polled. Confirmed live it also returns:
  `hex` (ICAO24, present on all), `desc` (full type name), `dbFlags` (bit 0 = 1 →
  military; `/v2/mil` returns 410/410 with `dbFlags & 1`), `emergency` (status
  string, e.g. `none`/`general`/`lifeguard`/`unlawful`), `squawk`. dbFlags is only
  populated for flagged aircraft (absent → not military).
- **planespotters.net public photo API** (`/pub/photos/reg/{reg}` and
  `/pub/photos/hex/{hex}`): free, no key, but **rejects generic User-Agents** —
  requires a descriptive UA with a contact URL (HTTP 403 otherwise). Returns
  `photos[].thumbnail` / `thumbnail_large` (`{src,size}`), `link`, `photographer`.
  Verified: `reg/D-AIMA` and `hex/3c4ad2` return a photo with a proper UA.

## Model + parser

`lib/data/aircraft.dart` — `Aircraft` gains:
- `hex` (String, ICAO24 lowercase, "" if missing) — photo fallback + alert identity.
- `desc` (String, full type description, "" if missing) — card subtitle.
- `isMilitary` (bool) — `(dbFlags & 1) != 0`.
- `isEmergency` (bool) — `squawk ∈ {7500, 7600, 7700}` OR the `emergency` field is
  present and not one of `{"", "none"}`.
- `distKm` (double?, null until computed) — set by the engine from the GPS center;
  used for the card distance and (optionally) display ordering.

Add `toJson()` / `fromJson()` producing/consuming only primitive-serializable
types (String/num/bool/null) so the list can cross the Android isolate boundary.
`copyWith` is extended to also carry `distKm` (alongside the existing
origin/dest). `track`/`squawk`/`registration` are retained (still encoded into the
BLE packet unchanged).

`lib/data/airplanes_client.dart` `parseAircraft` — extract the new fields:
- `hex`: `(item['hex'] as String?)?.toLowerCase() ?? ''`.
- `desc`: `(item['desc'] as String?)?.trim() ?? ''`.
- `isMilitary`: `((item['dbFlags'] as num?)?.toInt() ?? 0) & 1 != 0`.
- `isEmergency`: from the parsed `squawk` and `(item['emergency'] as String?)`.

The nearest-first sort + cap to `bleMaxAircraft` (10) is unchanged.

## Feeder → UI data path

`lib/service/gateway_engine.dart`:
- A compact, serializable view type carries each aircraft to the UI. Reuse
  `Aircraft.toJson/fromJson` (the fields the card needs — callsign, type, desc,
  distKm, origin, dest, registration, hex, isMilitary, isEmergency — are all on
  `Aircraft`). No separate view class is introduced (YAGNI).
- The engine stores the last cycle's enriched list (with `distKm` filled via
  `haversineKm(fix.lat, fix.lon, a.lat, a.lon)`), and `GatewayStatus` gains
  `final List<Aircraft> aircraft;` (default `const []`). `_emit()` includes it.

`lib/service/gateway_task_handler.dart` (Android): the status callback adds
`'aircraft': status.aircraft.map((a) => a.toJson()).toList()` to the
`sendDataToMain` map.

`lib/service/gateway_controller.dart` (Android `_onData`): when `data['aircraft']`
is a `List`, rebuild `List<Aircraft>` via `Aircraft.fromJson` and put it on the
emitted `GatewayStatus`. iOS path already forwards the engine's `GatewayStatus`
verbatim, so the list flows through unchanged.

## #9 Viewer UI

`lib/ui/home_screen.dart` becomes:
- A compact status header (Device / GPS / "N aircraft" in a single row or two) and
  the Start/Stop button (existing behavior preserved).
- An `Expanded` `ListView.builder` of `AircraftCard`s from `_status.aircraft`. When
  the list is empty, a centered hint ("Start feeding to see nearby aircraft").

`lib/ui/aircraft_card.dart` (new) — one card:
```
[ photo ]  CALLSIGN   [🚨]/[MIL]
[ 80px  ]  Type/desc · 12 km
[       ]  EGLL → KJFK · G-XLEA
           © Photographer / planespotters.net
```
- Leading photo: a fixed ~80px box. While the lookup is pending or on a miss, show
  a neutral placeholder (an airplane icon). On hit, `Image.network(thumbnailUrl)`.
- Title: callsign (or "------"); trailing badges: a red "EMG" chip when
  `isEmergency`, a "MIL" chip when `isMilitary`.
- Subtitle lines: `desc` (fallback `type`) + `distKm`; then route `origin → dest`
  (only when both present and differ) + registration.
- Attribution caption: `© {photographer} / planespotters.net` (required by the
  photo API terms). No tap-through (out of scope — avoids `url_launcher`).

`lib/data/photo_client.dart` (new) — `PhotoClient`:
- `Future<PhotoRef?> lookup({required String reg, required String hex})`: try
  `reg` first (if non-empty), then `hex` (if non-empty); the first hit wins.
- `PhotoRef { String thumbUrl; String photographer; String link; }`.
- Caches by a key (the reg or hex that produced the result), **including misses**
  (`null`) so a photoless aircraft isn't re-fetched.
- Uses a descriptive `User-Agent`
  (`flight-radar-esp32-companion/1.0 (+https://github.com/disclaimer8/flight-radar-esp32)`)
  and an injectable `http.Client` (for tests). 8 s timeout; any error → `null`
  (cached as a miss).
- Photos are fetched only in the UI (main isolate), lazily per visible card — never
  in the background feed cycle.

## #10 Emergency/Military push

`lib/service/alerts.dart` (new) — pure, host-tested:
- `bool isAlertWorthy(Aircraft a)` → `a.isEmergency || a.isMilitary`.
- `({List<Aircraft> newAlerts, Set<String> alerted}) computeNewAlerts(
   List<Aircraft> current, Set<String> previouslyAlerted)`:
  - qualifying = `current.where(isAlertWorthy)` with a non-empty `hex`;
  - `newAlerts` = qualifying whose `hex` is NOT in `previouslyAlerted`;
  - returned `alerted` = the set of qualifying hexes **in the current cycle** (so an
    aircraft that leaves and returns re-alerts; steady-state presence does not
    re-alert).
- `String alertTitle(Aircraft a)` / `String alertBody(Aircraft a)`:
  emergency → title `Emergency squawk`, body `🚨 {squawk}: {callsign}`; else
  military → title `Military aircraft`, body `{callsign} {type}`. (Emergency takes
  precedence when both.)

`lib/service/notification_service.dart` (new) — thin `flutter_local_notifications`
wrapper: `init()` (Android channel `flight_radar_alerts` + request permission;
iOS request alert permission), and `show(int id, String title, String body)`.
A stable id derived from the aircraft hex keeps repeat shows from stacking.

`lib/service/gateway_engine.dart`:
- Owns a `NotificationService` (init in `start()`, so it initializes in whichever
  isolate runs the engine — FGS isolate on Android, main isolate on iOS) and a
  `Set<String> _alerted`.
- In `_cycle`, after fetching the aircraft list, call `computeNewAlerts`; `show`
  each new alert; replace `_alerted` with the returned set. Detection runs on every
  cycle regardless of foreground/background, so alerts fire while feeding in the
  background. A notification failure never aborts the feed cycle (wrapped so the
  BLE send still happens).

`lib/ui/home_screen.dart` `_requestPermissions`: also ensure notification
permission (Android 13+ POST_NOTIFICATIONS is already handled for the FGS; add the
iOS alert permission request via the notification service / permission_handler).

## Testing (flutter_test)

- `parseAircraft`: `hex`/`desc` extraction; `isMilitary` true for `dbFlags:1`,
  false when absent or `dbFlags:8`; `isEmergency` true for `squawk:"7700"`, true
  for `emergency:"general"`, false for `squawk:"1200"` + `emergency:"none"`.
- `Aircraft.toJson`/`fromJson` round-trip (all new + existing fields, incl. nulls).
- `computeNewAlerts`: first sighting → alerted; same aircraft next cycle → no new
  alert; aircraft gone then back → re-alert; non-worthy aircraft never alert;
  empty-hex worthy aircraft skipped.
- `isAlertWorthy` / `alertTitle` / `alertBody` for emergency, military, both.
- `PhotoClient` with an injected fake `http.Client`: reg hit; reg miss → hex
  fallback hit; both miss → `null` cached (second lookup makes no HTTP call);
  request carries the descriptive User-Agent; non-200 → `null`.
- On-device: list shows photos + routes for nearby traffic; a notification fires
  for a military/emergency overflight (or by pointing the feed at an area known to
  have one, or temporarily relaxing the predicate for a smoke test).

## Out of scope

- Tapping a photo to open planespotters.net (no `url_launcher`).
- Fetching photos in the background feed cycle (UI/foreground only).
- A military on/off toggle (both categories always alert).
- Any BLE wire / firmware change (hex/military/emergency stay app-internal).
- Alert history/log, per-aircraft mute, or notification grouping.
- Distance-based viewer sort beyond the existing nearest-first order from the feed.

## Done criteria

- `cd companion && flutter test` green (incl. the new parser, JSON round-trip,
  alert-dedup, and PhotoClient tests) and `flutter analyze` clean.
- The home screen lists the fed aircraft with photo, type, distance, route, and
  registration; emergency/military cards show a badge.
- A new emergency or military aircraft in a feed cycle produces a local
  notification, on both Android (incl. background) and iOS; steady-state presence
  does not spam repeat notifications.
