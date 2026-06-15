# Non-blocking Wi-Fi portal coexisting with BLE provisioning

**Date:** 2026-06-15
**File touched:** `src/flight_ticker.ino` (Arduino layer only)

## Problem

On boot with no/failed Wi-Fi credentials the firmware opens the WiFiManager
captive portal via `connectWifi()` → `wm.autoConnect("FlightRadar-Setup")`, which
**blocks `loop()` for up to 180 s**. BLE keeps advertising and the Wi-Fi-config
characteristic's `onWrite` still buffers a packet (`g_wifiCfgReady = true`), but the
packet is only *applied* in `loop()` (`applyWifiConfig()`). Because `loop()` is
frozen inside WiFiManager, **provisioning the device's Wi-Fi from the companion app
over BLE does nothing until the 180 s portal timeout expires.** Same freeze happens
on the long-press on-demand portal (`startPortalOnDemand()`).

## Goal

Keep the browser captive portal, but make it **non-blocking** so that while the
device waits for Wi-Fi, BOTH paths work at the same time:
- browser → `FlightRadar-Setup` AP → `192.168.4.1`
- companion app → BLE write to `f1a90003` → device connects immediately

## Design (all in `src/flight_ticker.ino`)

### 1. WiFiManager promoted to file scope
WiFiManager and its parameters must outlive a single function call now (the portal
runs across many `loop()` iterations):
- `static WiFiManager g_wm;`
- `static WiFiManagerParameter g_latParam("lat", "Observer latitude", "", 15);`
- `static WiFiManagerParameter g_lonParam("lon", "Observer longitude", "", 15);`
- `static bool g_portalActive = false;`

One-time `wmInit()` called once in `setup()` (before `connectWifi()`):
`setConfigPortalBlocking(false)`, `setConfigPortalTimeout(180)`,
`addParameter(&g_latParam/&g_lonParam)`, AP callback → `drawSetupScreen()`,
save-params callback → parse + persist lat/lon (the existing logic).

A small helper `refreshPortalLatLon()` writes the current `g_obsLat/g_obsLon` into
the two params (via `setValue`) before opening the portal.

### 2. `connectWifi()` (boot)
Seed `config.h` creds as today, then `bool ok = g_wm.autoConnect("FlightRadar-Setup")`
— returns immediately in non-blocking mode. `g_portalActive = !ok`. `setup()`
continues and starts `netTask` regardless.

### 3. `startPortalOnDemand()` (long-press)
`refreshPortalLatLon()`, `drawSetupScreen()`, `g_wm.startConfigPortal("FlightRadar-Setup")`
(returns immediately), `g_portalActive = true`. No longer freezes `loop()`.

### 4. `loop()`
- Service: `if (g_portalActive) { if (!g_wm.process()) g_portalActive = false; }`
  (`process()` returns false when the portal closes — successful save+connect or
  timeout).
- Render gate: while `g_portalActive && WiFi.status() != WL_CONNECTED`, keep the
  setup screen (already drawn on portal open by the AP callback / on-demand) and
  skip the radar/detail/photo render. Once connected, the normal views resume.

### 5. `applyWifiConfig()` (BLE path) — with fail-recovery
At the start: if `g_portalActive`, `g_wm.stopConfigPortal(); g_portalActive = false;`
(tear down the AP for a clean STA connect). Keep the existing `WiFi.begin` +
12 s wait + `notifyWifiStatus`. **On failure, reopen the non-blocking portal**
(`refreshPortalLatLon()`, `g_wm.startConfigPortal(...)`, `drawSetupScreen()`,
`g_portalActive = true`) so the user is never locked out — they can retry over BLE
or via the browser.

### 6. `drawSetupScreen()`
Add a line indicating the app/BLE path also works (e.g. "or send from app"), so the
user knows waiting for the browser is optional.

## Coexistence

NimBLE (Bluetooth) and Wi-Fi AP/STA are independent and already coexist today
(BLE + STA + TLS run concurrently per the netTask design). The portal runs in AP
mode while BLE keeps advertising; `g_wifiCfgChar` `onWrite` buffers; `loop()` now
runs so `applyWifiConfig()` fires.

## Testing

The change is entirely in the Arduino-only `.ino` glue (no new pure functions), so
the host suite (`pio test -e native`) does not cover it; it must still pass
unchanged. On-device manual verification:
1. Boot with bad/empty creds → SETUP screen; browser portal reachable at
   `192.168.4.1`.
2. While on the SETUP screen, send Wi-Fi from the companion app over BLE → device
   connects **without** waiting for the 180 s timeout; radar resumes.
3. Long-press → portal opens, `loop()` keeps running; BLE provisioning still works.
4. BLE connect with a wrong password → "connect failed" notify, then the portal
   reopens automatically.

## Out of scope (YAGNI)

No new settings screens, no config flags, no changes to the BLE wire format or the
companion app.
