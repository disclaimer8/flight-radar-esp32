# BLE Wi-Fi Provisioning from the Mobile App — Design

> **Status:** approved design. Sub-project 5 of the flight-radar work. Adds a
> second Wi-Fi provisioning path: the companion app sends Wi-Fi credentials to the
> device over BLE (alongside the captive portal from sub-project 4). Built on the
> same `wifi-captive-portal` branch (merged together).

## Purpose

The captive portal (#8) lets a user provision the device's Wi-Fi from a phone
browser. This adds a smoother path: a section in the companion app where the user
types the SSID + password and taps **Send to device** — the credentials travel over
the existing BLE link to the device, which applies them, joins the network, and
reports the result back over BLE.

## Decisions (from brainstorm)

- **BLE transport:** a **new dedicated characteristic** (`wifi-config`, WRITE +
  NOTIFY) on the existing service — the aircraft-feed protocol is untouched.
- **App connection:** a **separate on-demand** BLE flow in the UI isolate
  (scan → connect → write → await notify → disconnect). The device is a
  single-central peripheral, so this requires the feeder to be **stopped** (the
  Send button is disabled while feeding).
- **Feedback:** **BLE notify ACK** — the device notifies a status (applying /
  connected+IP / failed); the app shows it.
- **UI:** a **provisioning section on the home screen** (under the status/list).

## BLE protocol (new characteristic)

New characteristic UUID `f1a90003-7e1d-4c2a-9b3f-1a2b3c4d5e6f` on the existing
service `f1a90001-...`, properties **WRITE + NOTIFY**.

### WRITE — credentials (app → device)

A small framed packet (single BLE write):

```
offset 0:  0x57 'W'        magic 0
offset 1:  0x43 'C'        magic 1
offset 2:  0x01            version (1)
offset 3:  ssidLen (u8)    1..32
offset 4:  ssid bytes      ssidLen bytes (UTF-8)
offset 4+ssidLen:        passLen (u8)   0..63 (0 allowed: open network)
offset 5+ssidLen:        pass bytes     passLen bytes (UTF-8)
```

Total ≤ 5 + 32 + 63 = 100 bytes. Parse rejects: wrong magic, wrong version,
truncated (declared length exceeds the buffer), `ssidLen == 0`, `ssidLen > 32`,
`passLen > 63`.

### NOTIFY — status (device → app)

```
offset 0:  status code (u8)   0 = applying, 1 = connected, 2 = failed
offset 1+: ASCII detail       connected → IP (e.g. "192.168.1.42"); failed → "" or a short reason
```

The device notifies `applying` immediately after a valid write, then `connected`
(with the IP) or `failed` after the connect attempt resolves.

Both formats are byte-exact across firmware C++ and Dart.

## Firmware (device peripheral)

`src/wifi_config_core.h` (new, Arduino-free, host-tested):

```cpp
struct WifiConfig { bool ok = false; std::string ssid; std::string pass; };
WifiConfig parseWifiConfig(const uint8_t* buf, size_t len);
```

`src/flight_ticker.ino`:
- In `setup()`, after the existing ingest characteristic, create the `wifi-config`
  characteristic (WRITE | WRITE_NR | NOTIFY) on the same service, with a
  `WifiConfigCallbacks` whose `onWrite` only copies bytes into a buffer + sets a
  `g_wifiCfgReady` flag (no work in the BLE callback — mirrors `IngestCallbacks`).
  Keep a global `NimBLECharacteristic*` to the char for notifying from `loop()`.
- In `loop()`, when `g_wifiCfgReady`: clear the flag, `parseWifiConfig`; if invalid,
  notify `failed`; if valid:
  - draw a "Configuring Wi-Fi…" LCD screen,
  - notify `applying`,
  - `WiFi.persistent(true); WiFi.begin(ssid, pass);` (persists to NVS, so the next
    boot's `autoConnect` reuses it),
  - wait up to ~12 s for `WL_CONNECTED` (a bounded busy-wait, acceptable for a
    deliberate one-shot provisioning action, like the captive portal's blocking),
  - notify `connected` + `WiFi.localIP()` on success, else `failed`.
- A small `notifyWifiStatus(uint8_t code, const String& detail)` helper builds the
  status payload and calls `notify()` on the characteristic.

Applying credentials integrates with the captive-portal flow: both persist to the
ESP32 Wi-Fi NVS, last-write-wins, and `autoConnect` uses whatever is stored.

## App (companion)

`lib/packet/wifi_config_packet.dart` (new):
- `Uint8List encodeWifiConfig(String ssid, String pass)` — builds the WRITE packet
  (magic + version + length-prefixed ssid/pass). Asserts/clamps ssid ≤32, pass ≤63.
- `WifiStatus parseWifiStatus(List<int> bytes)` — `{int code; String detail}` for the
  notify payload.

`lib/ble/wifi_provisioner.dart` (new) — `WifiProvisioner`, independent of
`BleManager`/the gateway, running in the UI (main) isolate:
- `Stream<ProvisionState> provision(String ssid, String pass)` (or a Future +
  status callback): scan for the service → connect → discover the `wifi-config`
  characteristic → `setNotifyValue(true)` + listen → write `encodeWifiConfig(...)`
  → map incoming notifications to states (`connecting`, `sending`, `applying`,
  `connected(ip)`, `failed(reason)`) → disconnect when terminal or on timeout.
- Uses `flutter_blue_plus` (already a dependency). Overall timeout (~30 s) so a
  missing device or stuck connect resolves to `failed`.

`lib/ui/home_screen.dart` — a provisioning section under the status/list:
- `SSID` + `Password` text fields and a **Send to device** button + a status line.
- The button is **disabled while the feeder is running** (`_running == true`) with a
  hint ("Stop feeding to configure device Wi-Fi") — the device's single BLE slot is
  held by the feeder.
- Tapping runs `WifiProvisioner.provision(...)`, streaming progress into the status
  line; the password field uses `obscureText`.

## Testing

- Unity (native): `parseWifiConfig` — valid packet (ssid+pass extracted); bad magic;
  bad version; truncated (declared length > buffer); `ssidLen == 0`; `ssidLen > 32`;
  `passLen > 63`; empty password (open network) accepted.
- flutter_test: `encodeWifiConfig` round-trips byte-exactly against a hand-built
  expected packet (and matches the firmware layout); `parseWifiStatus` decodes
  code + detail (connected+IP, failed, applying).
- On-device: from the app (feeder stopped) enter SSID + password → Send → the device
  shows "Configuring Wi-Fi…", joins the network, and the app shows
  "Connected <ip>"; a wrong password shows "Failed"; the provisioned network
  persists across a device reboot (autoConnect uses it, no portal).

## Security

The Wi-Fi password is sent over an unauthenticated, unencrypted BLE link — the same
threat model as the captive portal's open AP. BLE pairing/encryption is out of scope.

## Out of scope

- Scanning for nearby networks from the device (the SSID is typed by hand).
- Provisioning while the feeder is actively connected (single BLE slot — feeder must
  be stopped).
- Static IP / mDNS, multiple stored networks, BLE bonding/encryption.
- Changing the BLE aircraft-feed protocol (untouched).

## Done criteria

- `pio test -e native -f test_core` green (incl. the new `parseWifiConfig` cases);
  `cd companion && flutter test` green (incl. `encodeWifiConfig`/`parseWifiStatus`);
  `flutter analyze` clean; `pio run -e esp32-s3` compiles.
- On device: the app provisions Wi-Fi over BLE end-to-end (credentials applied,
  device joins the network, status reported back), the result persists across
  reboot, and the Send button is correctly gated on the feeder being stopped.
