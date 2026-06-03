# Detail Enrichment — Route + Registration + Operator — Design

> **Status:** approved design. Sub-project 1 of the 4-10 feature batch. Adds three
> fields to the tap-detail view, on both Wi-Fi and BLE paths (BLE wire → v3).

## Purpose

Enrich the detail carousel (tap a blip) with:
- **Registration** — the aircraft tail number (e.g. `D-AIMA`).
- **Operator** — the airline (ICAO code from the callsign, e.g. `BAW`).
- **Route** — origin → destination airports (e.g. `EGLL → KJFK`).

Decided in brainstorm: full parity on both data paths (BLE wire bumped to v3); route
via **hexdb.io** (free, no auth — chosen because the FR24 paid account is out of
credits); operator derived free from the callsign (no lookup).

## Data sources (verified)

- **Registration:** airplanes.live `r` field (already polled on Wi-Fi; ADSBExchange-v2 format).
- **Route:** `https://hexdb.io/api/v1/route/icao/<callsign>` → `{"flight":"BAW117","route":"EGLL-KJFK",...}`. Parse `route`, split on `-`: origin = first ICAO, dest = last. Unknown callsign → no route (omit). Verified live: `BAW117 → EGLL-KJFK`.
- **Operator:** the first 3 chars of the callsign when they are letters (the airline ICAO code, e.g. `BAW117` → `BAW`). Pure derivation; no lookup. Tail-number callsigns (e.g. `N12345`) yield no operator.

## New `Aircraft` fields

Firmware `flight_core.h` and Dart `aircraft.dart` `Aircraft` gain:
- `registration` (string, "" if missing)
- `origin` (string, ICAO, "" if unknown)
- `dest` (string, ICAO, "" if unknown)

Operator is NOT stored — derived from `callsign` at render time.

## Route lookup mechanism (lazy on Wi-Fi, prefetch on BLE)

A route lookup is per-callsign and cached (route doesn't change during a flight).

- **Wi-Fi path (firmware):** `parseNearest` fills `registration` from `r`; leaves
  origin/dest empty. When the detail view shows an aircraft whose route isn't
  cached, the firmware does ONE blocking `hexdb.io` HTTPS GET (reusing the
  `WiFiClientSecure`+`setInsecure` pattern), caches `callsign → (origin,dest)` in a
  global map (survives polls), and fills origin/dest. Subsequent opens are instant.
  A cache-miss fetch briefly freezes the frame (~like the API poll) — acceptable.
- **BLE path (app):** for each aircraft it sends, the app looks up hexdb (cached by
  callsign in the app), and has `registration` from `r`; it encodes all three into
  the v3 packet. The firmware's `parseBlePacket` fills them directly — no device
  lookup needed (the device has no internet on the BLE path).

## BLE wire format v3

The v2 record is 32 B; v3 grows to **48 B** by appending three fixed-width ASCII
string fields, and bumps the version. Empty string = field absent (no new valid
flags needed — unlike the numeric track/squawk).

- `BLE_VERSION = 3`, `BLE_RECORD_SIZE = 48`, **`BLE_MAX_AIRCRAFT 15 → 10`** (a full
  16-record v3 packet would be 12+16·48 = 780 B ≫ 514 B ATT payload @ MTU 517;
  10 records = 12+10·48 = **492 B** fits; display cap stays `MAX_AIRCRAFT` = 10).
  `BLE_MAX_PACKET = 492`.
- Record layout: bytes 0–31 unchanged from v2, then **registration[32:40]** (8 B
  ascii, space-padded), **origin ICAO[40:44]** (4 B), **dest ICAO[44:48]** (4 B).
- Both sides controlled (mono-repo) → the firmware parser may reject non-v3.

## Pure helpers (host-tested)

- `parseHexdbRoute(routeStr) → (origin, dest)`: split `"EGLL-KJFK"` on `-`;
  origin = first segment, dest = last; both "" on malformed/empty. (firmware
  `flight_core.h` or a small util; Dart mirror.)
- `airlineCode(callsign) → string`: first 3 chars if callsign length ≥ 3 and those
  3 are A–Z letters, else "". **Firmware only** (`render_core.h`) — the operator is
  rendered on the device's detail view; the app (a feeder with no detail UI) does
  not need it. The app DOES need `parseHexdbRoute` (for its prefetch).

## Detail view (firmware `drawDetail`)

Add up to three short lines to the existing detail layout (callsign, type+compass,
distance, alt, speed): **Reg `<registration>`**, **Op `<airlineCode>`**, **Route
`<origin> → <dest>`** — each shown only when its value is non-empty. Keep the lean
layout; the round 240×240 screen is tight, so place them compactly (small font).

## Testing (TDD, host)

Pure unit tests both suites:
- `parseHexdbRoute` (firmware Unity + Dart): valid `EGLL-KJFK`, single-segment,
  empty/malformed.
- `airlineCode` (firmware only): `BAW117 → BAW`, `N12345 → ""` (digits), short callsign → "".
- `parseNearest` (firmware) + `parseAircraft` (app): extract `registration` from
  `r`.
- `parseBlePacket` v3 round-trip (firmware) + `encodePacket` v3 (Dart): registration
  @32, origin @40, dest @44, byte-identical; existing tests updated to 48-byte
  records + cap 10.
- The hexdb HTTP client (firmware + app) is glue — verified on-device, not unit
  tested (the pure parse is).

On-device: tap a blip on Wi-Fi → Reg/Op/Route appear (route fetched from hexdb);
on BLE (`ble_send.py` v3 with sample reg/route) → same fields from the packet.

## Out of scope

- Full operator/airline NAMES (only the ICAO code; names need an airline DB).
- FR24 (account out of credits; token kept out of git for possible later use in
  sub-project 3's phone photos/airline).
- Multi-leg route display (show first→last only).
- Caching TTL/expiry tuning (a simple per-session callsign cache; routes are stable
  within a flight).

## Done criteria

- `pio test -e native -f test_core` and `flutter test` green (incl. v3 round-trip +
  the two pure helpers).
- `pio run -e esp32-s3` compiles; `ble_send.py` emits v3.
- On device (both paths): tapping an aircraft shows its registration, operator
  (airline code), and route origin→destination, with the route from hexdb.io on
  Wi-Fi and from the v3 packet on BLE.
