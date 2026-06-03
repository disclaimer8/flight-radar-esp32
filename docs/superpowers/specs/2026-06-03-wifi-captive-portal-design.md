# WiFiManager Captive Portal — Design

> **Status:** approved design. Sub-project 4 (final) of the 4-10 feature batch
> (feature #8). Replaces compile-time Wi-Fi credentials with a captive-portal
> setup flow, and makes the observer location configurable from the phone.

## Purpose

Today Wi-Fi credentials (`WIFI_SSID`/`WIFI_PASS`) and the observer location
(`MY_LAT`/`MY_LON`) are compile-time `#define`s in the gitignored `config.h`;
`connectWifi()` does a single `WiFi.begin(WIFI_SSID, WIFI_PASS)` with a 20 s
timeout. Relocating the radar or changing networks means editing `config.h` and
re-flashing.

This sub-project adds a **captive portal** (tzapu/WiFiManager): on boot, if no
saved network connects, the device raises a `FlightRadar-Setup` access point with
a captive portal where the user picks a network, enters the password, and sets the
observer latitude/longitude — all from a phone browser, no re-flash. Settings
persist in NVS. A long-press on the radar reopens the portal on demand.

## Decisions (from brainstorm)

- **Library:** `tzapu/WiFiManager` (AP + captive portal + network scan + credential
  persistence + custom parameters; built-in WebServer + DNSServer, not async).
- **Configurable via portal:** Wi-Fi credentials **and** observer lat/lon (custom
  parameters, persisted to NVS).
- **Portal access:** automatic (when saved credentials fail to connect) **and**
  on-demand via a long-press (`TG_LONG`) on the radar.
- **`config.h` Wi-Fi defines:** kept as a **seed** — if NVS has no stored
  credentials and `config.h` has them, they are tried before opening the portal.
  `MY_LAT`/`MY_LON` stay as the location defaults.

## Observer location → runtime + NVS

New globals `g_obsLat` / `g_obsLon` (double) **replace `MY_LAT`/`MY_LON`** at every
use site:
- the `pollApi()` URL center and the `parseNearest(...)` center,
- the `g_centerLat` / `g_centerLon` initial values,
- the detail-view bearing center.

`setup()` loads them from `Preferences` (namespace `"radar"` — reused from the
range-zoom feature — keys `"lat"` / `"lon"` via `getDouble`), defaulting to
`MY_LAT` / `MY_LON` when unset. The portal's save-params callback writes them.

## connectWifi() rewrite

```text
WiFiManager wm
wm.setConfigPortalTimeout(180)              // 3 min, then boot offline (BLE fallback)
prefill lat/lon WiFiManagerParameter from g_obsLat/g_obsLon
wm.addParameter(&latParam); wm.addParameter(&lonParam)
wm.setAPCallback(-> drawSetupScreen())      // portal opened: show the LCD setup screen
wm.setSaveParamsCallback(-> parse+validate+persist lat/lon)
// seed: if no stored creds and config has them, persist config creds first
if WiFi.SSID().isEmpty() and strlen(WIFI_SSID) > 0:
    WiFi.persistent(true); WiFi.begin(WIFI_SSID, WIFI_PASS)
wm.autoConnect("FlightRadar-Setup")         // try saved/seed; else portal (blocks)
```

`autoConnect` connects with stored credentials, or raises the AP + captive portal
and blocks until configured or the timeout elapses. On timeout it returns false and
the device proceeds offline — the existing `WiFi.status() == WL_CONNECTED` guards in
`loop()`/`pollApi()` already handle the offline case, and the BLE fallback path is
unaffected.

The save-params callback reads the two parameters, runs them through
`parseLatLon` (below); on success it updates `g_obsLat`/`g_obsLon` and writes them to
NVS. Invalid input is ignored (keeps the previous values).

## On-demand portal (TG_LONG)

In `handleTouch()`'s `RADAR` branch, add `TG_LONG` → a `startPortalOnDemand()` helper
that constructs a `WiFiManager`, adds the same lat/lon parameters + callbacks, and
calls `wm.startConfigPortal("FlightRadar-Setup")`. This blocks `loop()` while the
portal is active (the radar freezes — an acceptable trade for a deliberate
reconfiguration) and draws the setup screen via the AP callback. After it returns
(configured or timed out), normal operation resumes and Wi-Fi reconnects.

## LCD setup screen

`drawSetupScreen()` renders, centered on the round 240×240 panel: a `SETUP` title,
`Join Wi-Fi:`, the AP name `FlightRadar-Setup`, and `then open 192.168.4.1`. It is
invoked from the AP callback for both the boot-time and on-demand portals.

## Pure helper (host-tested)

New `src/coord_core.h` (Arduino-free, included by the test suite):

```cpp
// Parse two coordinate strings; write to lat/lon and return true only when both
// are numeric and in range (lat [-90,90], lon [-180,180]). On any failure the
// out-params are left untouched and false is returned.
bool parseLatLon(const char* latStr, const char* lonStr, double& lat, double& lon);
```

This is the testable surface; WiFiManager, the portal, the LCD screen, and NVS are
glue verified on-device.

## Dependency / coexistence

Add `tzapu/WiFiManager@^2.0.17` to `lib_deps` for the `esp32-s3` env only (the
`native` test env keeps only ArduinoJson — `coord_core.h` is Arduino-free). The
portal uses the built-in `WebServer` + `DNSServer`. NimBLE is initialized before
`connectWifi()` in `setup()`, so the portal's AP + webserver coexist with the BLE
peripheral and the 115 KB sprite — Wi-Fi/BLE coexistence is supported on the S3 and
verified previously; the on-device acceptance step confirms SRAM headroom with the
portal running.

## Testing

- Unity (native): `parseLatLon` — valid pair; boundary values (±90 / ±180 accepted);
  out-of-range (91, 181 rejected); garbage / empty strings rejected; out-params
  untouched on failure.
- On-device:
  - Fresh provisioning: with no usable saved network, the `FlightRadar-Setup` portal
    appears; selecting a network + entering lat/lon connects and the radar centers on
    the entered coordinates.
  - Long-press on the radar reopens the portal; changing the network reconnects
    without re-flashing.
  - Seed: a device with `config.h` credentials and empty NVS connects to that network
    without the portal.
  - Timeout: leaving the portal unconfigured for 180 s boots to the radar offline
    (NO LINK / BLE fallback), no crash.

## Out of scope

- mDNS / static IP.
- Configuring other tunables (radius, poll interval, sweep) via the portal.
- A custom-designed portal page (WiFiManager's default UI is used).
- Changing the AP password (the setup AP is open).
- Persisting/validating coordinates beyond range checks (no geocoding).

## Done criteria

- `pio test -e native -f test_core` green (incl. the new `parseLatLon` cases);
  existing cases still pass.
- `pio run -e esp32-s3` compiles with WiFiManager linked.
- On device: the captive portal provisions Wi-Fi + location, the radar uses the
  entered coordinates, a long-press reopens the portal, the `config.h` seed still
  works, and an unconfigured portal times out into offline operation.
