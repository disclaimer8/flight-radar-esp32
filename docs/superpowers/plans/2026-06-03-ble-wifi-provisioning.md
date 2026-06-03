# BLE Wi-Fi Provisioning from the Mobile App Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let the companion app send Wi-Fi credentials to the device over a new BLE characteristic; the device applies them, joins the network, and reports status back over BLE notify.

**Architecture:** A new `wifi-config` BLE characteristic (WRITE + NOTIFY) on the existing service. App side: a standalone `WifiProvisioner` (scan → connect → write creds → await notify → disconnect, feeder must be stopped) driven from a provisioning section on the home screen. The wire format (`parseWifiConfig` firmware / `encodeWifiConfig` Dart) is byte-exact and host-tested; BLE/WiFi/UI are on-device glue.

**Tech Stack:** C++ (Arduino/ESP32-S3), NimBLE, TFT_eSPI; Flutter (`flutter_blue_plus`); PlatformIO Unity + flutter_test.

**Spec:** `docs/superpowers/specs/2026-06-03-ble-wifi-provisioning-design.md`

Firmware compiles with `/opt/homebrew/bin/pio run -e esp32-s3`; app from `companion/` with `flutter test` / `flutter analyze`. This is built on the existing `wifi-captive-portal` branch.

---

## File structure

- `src/wifi_config_core.h` (new) — Arduino-free `parseWifiConfig` (host-tested). **Task 1.**
- `test/test_core/test_main.cpp` — `parseWifiConfig` tests. **Task 1.**
- `companion/lib/packet/wifi_config_packet.dart` (new) — `encodeWifiConfig` + `parseWifiStatus`. **Task 2.**
- `companion/test/wifi_config_packet_test.dart` (new) — byte-parity tests. **Task 2.**
- `src/flight_ticker.ino` — 2nd characteristic + apply + notify. **Task 3.**
- `companion/lib/ble/wifi_provisioner.dart` (new) — on-demand provisioning BLE flow. **Task 4.**
- `companion/lib/ui/home_screen.dart` — provisioning section. **Task 5.**
- Full verify + on-device. **Task 6.**

Tasks 1-2 are pure TDD. Tasks 3-5 are glue (compile / analyze / widget test). The wire format in Task 1 and Task 2 MUST match byte-for-byte.

---

### Task 1: parseWifiConfig pure helper (wifi_config_core.h)

**Files:** Create `src/wifi_config_core.h`; Test `test/test_core/test_main.cpp`.

- [ ] **Step 1: Write failing test**

Add this test in `test/test_core/test_main.cpp` immediately before `void setUp(void) {}`:

```cpp
void test_parse_wifi_config(void) {
    // "WC" + ver1 + ssidLen5 "MyNet" + passLen6 "secret"
    uint8_t good[] = {0x57,0x43,0x01, 5,'M','y','N','e','t', 6,'s','e','c','r','e','t'};
    WifiConfig c = parseWifiConfig(good, sizeof(good));
    TEST_ASSERT_TRUE(c.ok);
    TEST_ASSERT_EQUAL_STRING("MyNet", c.ssid.c_str());
    TEST_ASSERT_EQUAL_STRING("secret", c.pass.c_str());

    // empty password (open network) accepted
    uint8_t open[] = {0x57,0x43,0x01, 2,'A','P', 0};
    WifiConfig o = parseWifiConfig(open, sizeof(open));
    TEST_ASSERT_TRUE(o.ok);
    TEST_ASSERT_EQUAL_STRING("AP", o.ssid.c_str());
    TEST_ASSERT_EQUAL_STRING("", o.pass.c_str());

    // bad magic
    uint8_t badmagic[] = {0x00,0x43,0x01, 2,'A','P', 0};
    TEST_ASSERT_FALSE(parseWifiConfig(badmagic, sizeof(badmagic)).ok);
    // bad version
    uint8_t badver[] = {0x57,0x43,0x09, 2,'A','P', 0};
    TEST_ASSERT_FALSE(parseWifiConfig(badver, sizeof(badver)).ok);
    // ssidLen 0
    uint8_t ssid0[] = {0x57,0x43,0x01, 0, 0};
    TEST_ASSERT_FALSE(parseWifiConfig(ssid0, sizeof(ssid0)).ok);
    // truncated: declares ssidLen 5 but only 2 ssid bytes present
    uint8_t trunc[] = {0x57,0x43,0x01, 5,'M','y'};
    TEST_ASSERT_FALSE(parseWifiConfig(trunc, sizeof(trunc)).ok);
    // ssidLen > 32
    uint8_t bigssid[] = {0x57,0x43,0x01, 33,'x'};
    TEST_ASSERT_FALSE(parseWifiConfig(bigssid, sizeof(bigssid)).ok);
    // passLen > 63
    uint8_t bigpass[] = {0x57,0x43,0x01, 1,'A', 64,'x'};
    TEST_ASSERT_FALSE(parseWifiConfig(bigpass, sizeof(bigpass)).ok);
}
```

Register it in `main()` after `RUN_TEST(test_parse_lat_lon);`:

```cpp
    RUN_TEST(test_parse_wifi_config);
```

Add the include after the existing `#include "../../src/coord_core.h"`:

```cpp
#include "../../src/wifi_config_core.h"
```

- [ ] **Step 2: Run test to verify it fails**

Run: `pio test -e native -f test_core`
Expected: FAIL to compile — `parseWifiConfig` / `wifi_config_core.h` missing.

- [ ] **Step 3: Implement**

Create `src/wifi_config_core.h`:

```cpp
#pragma once
#include <cstdint>
#include <cstddef>
#include <string>

// Wi-Fi provisioning packet (app -> device over BLE). Little-endian / byte fields.
constexpr uint8_t WIFICFG_MAGIC0  = 0x57; // 'W'
constexpr uint8_t WIFICFG_MAGIC1  = 0x43; // 'C'
constexpr uint8_t WIFICFG_VERSION = 1;
constexpr size_t  WIFICFG_MAX_SSID = 32;
constexpr size_t  WIFICFG_MAX_PASS = 63;

struct WifiConfig {
    bool ok = false;
    std::string ssid;
    std::string pass;
};

// Parse "WC" + ver + ssidLen + ssid + passLen + pass. Returns ok=false on wrong
// magic/version, ssidLen 0 or >32, passLen >63, or a truncated buffer.
inline WifiConfig parseWifiConfig(const uint8_t* buf, size_t len) {
    WifiConfig c;
    if (!buf || len < 4) return c;                       // need magic(2)+ver+ssidLen
    if (buf[0] != WIFICFG_MAGIC0 || buf[1] != WIFICFG_MAGIC1) return c;
    if (buf[2] != WIFICFG_VERSION) return c;
    size_t ssidLen = buf[3];
    if (ssidLen == 0 || ssidLen > WIFICFG_MAX_SSID) return c;
    if (len < 4 + ssidLen + 1) return c;                 // need ssid + passLen byte
    size_t passOff = 4 + ssidLen;
    size_t passLen = buf[passOff];
    if (passLen > WIFICFG_MAX_PASS) return c;
    if (len < passOff + 1 + passLen) return c;           // truncated pass
    c.ssid.assign(reinterpret_cast<const char*>(buf + 4), ssidLen);
    c.pass.assign(reinterpret_cast<const char*>(buf + passOff + 1), passLen);
    c.ok = true;
    return c;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `pio test -e native -f test_core`
Expected: PASS — all cases incl. `test_parse_wifi_config`.

- [ ] **Step 5: Commit**

```bash
git add src/wifi_config_core.h test/test_core/test_main.cpp
git commit -m "feat(wifi): parseWifiConfig BLE provisioning packet parser + tests"
```

---

### Task 2: Dart encodeWifiConfig + parseWifiStatus

**Files:** Create `companion/lib/packet/wifi_config_packet.dart`; Test `companion/test/wifi_config_packet_test.dart`.

Work from `companion/`.

- [ ] **Step 1: Write failing test**

Create `companion/test/wifi_config_packet_test.dart`:

```dart
import 'package:flutter_test/flutter_test.dart';
import 'package:flight_radar_companion/packet/wifi_config_packet.dart';

void main() {
  test('encodeWifiConfig builds the byte-exact WC packet', () {
    final bytes = encodeWifiConfig('MyNet', 'secret');
    // "WC" + ver1 + ssidLen5 "MyNet" + passLen6 "secret"
    expect(bytes, [0x57, 0x43, 0x01, 5, 77, 121, 78, 101, 116, 6, 115, 101, 99, 114, 101, 116]);
  });

  test('encodeWifiConfig supports an empty password (open network)', () {
    final bytes = encodeWifiConfig('AP', '');
    expect(bytes, [0x57, 0x43, 0x01, 2, 65, 80, 0]);
  });

  test('parseWifiStatus decodes code + detail', () {
    expect(parseWifiStatus([0]).code, 0);              // applying
    final ok = parseWifiStatus([1, 49, 57, 50, 46, 49]); // "192.1"
    expect(ok.code, 1);
    expect(ok.detail, '192.1');
    final fail = parseWifiStatus([2]);
    expect(fail.code, 2);
    expect(fail.detail, '');
    expect(parseWifiStatus([]).code, 2); // empty payload treated as failed
  });
}
```

- [ ] **Step 2: Run, verify fail**

Run: `flutter test test/wifi_config_packet_test.dart`
Expected: FAIL — file missing.

- [ ] **Step 3: Implement**

Create `companion/lib/packet/wifi_config_packet.dart`:

```dart
import 'dart:convert';
import 'dart:typed_data';

// Mirror of src/wifi_config_core.h (byte-exact).
const int wifiCfgMagic0 = 0x57; // 'W'
const int wifiCfgMagic1 = 0x43; // 'C'
const int wifiCfgVersion = 1;
const int wifiCfgMaxSsid = 32;
const int wifiCfgMaxPass = 63;

/// Build the WRITE packet: "WC" + ver + ssidLen + ssid + passLen + pass.
/// SSID is clamped to 32 bytes, password to 63 (the WPA max).
Uint8List encodeWifiConfig(String ssid, String pass) {
  var s = utf8.encode(ssid);
  var p = utf8.encode(pass);
  if (s.length > wifiCfgMaxSsid) s = s.sublist(0, wifiCfgMaxSsid);
  if (p.length > wifiCfgMaxPass) p = p.sublist(0, wifiCfgMaxPass);
  final out = BytesBuilder();
  out.add([wifiCfgMagic0, wifiCfgMagic1, wifiCfgVersion, s.length]);
  out.add(s);
  out.add([p.length]);
  out.add(p);
  return out.toBytes();
}

/// Decoded NOTIFY status: code (0 applying, 1 connected, 2 failed) + ASCII detail.
class WifiStatus {
  final int code;
  final String detail;
  const WifiStatus(this.code, this.detail);
}

WifiStatus parseWifiStatus(List<int> bytes) {
  if (bytes.isEmpty) return const WifiStatus(2, ''); // empty = failed
  final detail = bytes.length > 1 ? utf8.decode(bytes.sublist(1), allowMalformed: true) : '';
  return WifiStatus(bytes[0], detail);
}
```

- [ ] **Step 4: Run, verify pass**

Run: `flutter test test/wifi_config_packet_test.dart`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add companion/lib/packet/wifi_config_packet.dart companion/test/wifi_config_packet_test.dart
git commit -m "feat(app): encodeWifiConfig + parseWifiStatus (byte-exact with firmware)"
```

---

### Task 3: Firmware — wifi-config characteristic + apply + notify

**Files:** Modify `src/flight_ticker.ino`.

Arduino glue; verify by `pio run -e esp32-s3`.

- [ ] **Step 1: Include the parser**

In `src/flight_ticker.ino`, after `#include "coord_core.h"`, add:

```cpp
#include "wifi_config_core.h"
```

- [ ] **Step 2: Add the wifi-config UUID constant**

In `src/flight_ticker.ino`, next to the existing `BLE_CHAR_UUID` definition, add:

```cpp
static const char* BLE_WIFICFG_UUID = "f1a90003-7e1d-4c2a-9b3f-1a2b3c4d5e6f";
```

- [ ] **Step 3: Add the buffer/flag/char globals + callbacks**

In `src/flight_ticker.ino`, immediately after the `IngestCallbacks` class (which ends near the `g_blePacketReady = true;` block), add:

```cpp
static uint8_t  g_wifiCfgBuf[128];
volatile size_t g_wifiCfgLen = 0;
volatile bool   g_wifiCfgReady = false;
NimBLECharacteristic* g_wifiCfgChar = nullptr;

// Receives a Wi-Fi provisioning packet from the app. Like IngestCallbacks, the
// write callback only buffers + flags; loop() does the apply (off the BLE task).
class WifiConfigCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c) override {
        std::string v = c->getValue();
        size_t n = v.size();
        if (n > sizeof(g_wifiCfgBuf)) n = sizeof(g_wifiCfgBuf);
        std::memcpy(g_wifiCfgBuf, v.data(), n);
        g_wifiCfgLen = n;
        g_wifiCfgReady = true;
    }
};
```

- [ ] **Step 4: Add the notify + apply helpers**

In `src/flight_ticker.ino`, immediately above `void setup()`, add:

```cpp
// Notify the app of provisioning status: 1 code byte + ASCII detail (IP / reason).
void notifyWifiStatus(uint8_t code, const String& detail) {
    if (!g_wifiCfgChar) return;
    uint8_t buf[64];
    buf[0] = code;
    size_t dlen = detail.length();
    if (dlen > sizeof(buf) - 1) dlen = sizeof(buf) - 1;
    std::memcpy(buf + 1, detail.c_str(), dlen);
    g_wifiCfgChar->setValue(buf, 1 + dlen);
    g_wifiCfgChar->notify();
}

// Apply a received Wi-Fi provisioning packet: parse, join, persist, report.
// Bounded blocking wait (~12s) is acceptable for a deliberate one-shot action.
void applyWifiConfig() {
    WifiConfig cfg = parseWifiConfig(g_wifiCfgBuf, g_wifiCfgLen);
    if (!cfg.ok) { notifyWifiStatus(2, "bad config"); return; }
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.drawString("Configuring", CX, 100, 4);
    tft.drawString("Wi-Fi...", CX, 140, 4);
    notifyWifiStatus(0, "");                 // applying
    WiFi.persistent(true);
    WiFi.begin(cfg.ssid.c_str(), cfg.pass.c_str());
    unsigned long t = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t < 12000) delay(100);
    if (WiFi.status() == WL_CONNECTED) {
        notifyWifiStatus(1, WiFi.localIP().toString());
    } else {
        notifyWifiStatus(2, "connect failed");
    }
}
```

- [ ] **Step 5: Create the characteristic in setup()**

In `src/flight_ticker.ino` `setup()`, between the existing `bleCh->setCallbacks(new IngestCallbacks());` line and `bleSvc->start();`, add:

```cpp
    g_wifiCfgChar = bleSvc->createCharacteristic(
        BLE_WIFICFG_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::NOTIFY);
    g_wifiCfgChar->setCallbacks(new WifiConfigCallbacks());
```

- [ ] **Step 6: Handle the flag in loop()**

In `src/flight_ticker.ino` `loop()`, immediately after the existing `if (g_blePacketReady) { ... }` block, add:

```cpp
    if (g_wifiCfgReady) {
        g_wifiCfgReady = false;
        applyWifiConfig();
    }
```

- [ ] **Step 7: Compile**

Run: `pio run -e esp32-s3`
Expected: SUCCESS.

- [ ] **Step 8: Commit**

```bash
git add src/flight_ticker.ino
git commit -m "feat(wifi): BLE wifi-config characteristic — apply creds + notify status"
```

---

### Task 4: App — WifiProvisioner BLE flow

**Files:** Create `companion/lib/ble/wifi_provisioner.dart`.

Arduino-free Dart glue (BLE); verify by `flutter analyze` + `flutter test` (no regression — BLE itself is on-device).

- [ ] **Step 1: Implement**

Create `companion/lib/ble/wifi_provisioner.dart`:

```dart
import 'dart:async';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import '../packet/wifi_config_packet.dart';

enum ProvPhase { idle, connecting, sending, applying, connected, failed }

class ProvState {
  final ProvPhase phase;
  final String detail;
  const ProvState(this.phase, [this.detail = '']);
}

/// On-demand BLE Wi-Fi provisioning, independent of the feeder. Scans for the
/// device, connects, writes the credentials to the wifi-config characteristic,
/// maps the status notifications to [ProvState]s, then disconnects. The device is
/// a single-central peripheral, so the feeder must be stopped before calling this.
class WifiProvisioner {
  static final Guid serviceUuid = Guid('f1a90001-7e1d-4c2a-9b3f-1a2b3c4d5e6f');
  static final Guid wifiCfgUuid = Guid('f1a90003-7e1d-4c2a-9b3f-1a2b3c4d5e6f');

  final _states = StreamController<ProvState>.broadcast();
  Stream<ProvState> get states => _states.stream;

  Future<void> provision(String ssid, String pass) async {
    BluetoothDevice? device;
    StreamSubscription<List<int>>? notifySub;
    final done = Completer<void>();
    try {
      _emit(const ProvState(ProvPhase.connecting));
      device = await _scanForDevice();
      if (device == null) {
        _emit(const ProvState(ProvPhase.failed, 'device not found'));
        return;
      }
      await device.connect(license: License.nonprofit, timeout: const Duration(seconds: 15));
      final services = await device.discoverServices();
      BluetoothCharacteristic? ch;
      for (final s in services) {
        if (s.uuid == serviceUuid) {
          for (final c in s.characteristics) {
            if (c.uuid == wifiCfgUuid) ch = c;
          }
        }
      }
      if (ch == null) {
        _emit(const ProvState(ProvPhase.failed, 'characteristic missing'));
        return;
      }

      await ch.setNotifyValue(true);
      notifySub = ch.onValueReceived.listen((bytes) {
        final st = parseWifiStatus(bytes);
        if (st.code == 0) {
          _emit(const ProvState(ProvPhase.applying));
        } else if (st.code == 1) {
          _emit(ProvState(ProvPhase.connected, st.detail));
          if (!done.isCompleted) done.complete();
        } else {
          _emit(ProvState(ProvPhase.failed, st.detail));
          if (!done.isCompleted) done.complete();
        }
      });

      _emit(const ProvState(ProvPhase.sending));
      await ch.write(encodeWifiConfig(ssid, pass), withoutResponse: false);

      // Wait for a terminal notification, or time out (the device's join attempt
      // is bounded ~12s; allow margin).
      await done.future.timeout(const Duration(seconds: 30), onTimeout: () {
        _emit(const ProvState(ProvPhase.failed, 'timeout'));
      });
    } catch (e) {
      _emit(ProvState(ProvPhase.failed, e.toString()));
    } finally {
      await notifySub?.cancel();
      try { await device?.disconnect(); } catch (_) {}
    }
  }

  Future<BluetoothDevice?> _scanForDevice() async {
    await FlutterBluePlus.adapterState.where((s) => s == BluetoothAdapterState.on).first;
    final completer = Completer<BluetoothDevice?>();
    final sub = FlutterBluePlus.onScanResults.listen((results) {
      if (results.isNotEmpty && !completer.isCompleted) completer.complete(results.first.device);
    });
    await FlutterBluePlus.startScan(withServices: [serviceUuid], timeout: const Duration(seconds: 15));
    final device = await completer.future
        .timeout(const Duration(seconds: 16), onTimeout: () => null);
    await sub.cancel();
    await FlutterBluePlus.stopScan();
    return device;
  }

  void _emit(ProvState s) {
    if (!_states.isClosed) _states.add(s);
  }

  void dispose() {
    _states.close();
  }
}
```

- [ ] **Step 2: Verify**

Run: `flutter analyze` → no new issues. Then `flutter test` → existing pass (the import compiles).

- [ ] **Step 3: Commit**

```bash
git add companion/lib/ble/wifi_provisioner.dart
git commit -m "feat(app): WifiProvisioner — on-demand BLE Wi-Fi provisioning flow"
```

---

### Task 5: App — provisioning section on the home screen

**Files:** Modify `companion/lib/ui/home_screen.dart`.

Glue; verify by `flutter test` + `flutter analyze`.

- [ ] **Step 1: Imports + state**

In `companion/lib/ui/home_screen.dart`, add imports:

```dart
import '../ble/wifi_provisioner.dart';
```

In `_HomeScreenState`, after `final _photos = PhotoClient();`, add:

```dart
  final _ssidCtrl = TextEditingController();
  final _passCtrl = TextEditingController();
  final _provisioner = WifiProvisioner();
  String _provStatus = '';
  bool _provisioning = false;
```

Add a provisioning method to `_HomeScreenState`:

```dart
  Future<void> _sendWifi() async {
    if (_ssidCtrl.text.isEmpty || _provisioning) return;
    setState(() { _provisioning = true; _provStatus = 'Connecting to device…'; });
    final sub = _provisioner.states.listen((s) {
      if (!mounted) return;
      setState(() {
        switch (s.phase) {
          case ProvPhase.connecting: _provStatus = 'Connecting to device…'; break;
          case ProvPhase.sending:    _provStatus = 'Sending credentials…'; break;
          case ProvPhase.applying:   _provStatus = 'Device joining Wi-Fi…'; break;
          case ProvPhase.connected:  _provStatus = 'Connected: ${s.detail}'; break;
          case ProvPhase.failed:     _provStatus = 'Failed: ${s.detail}'; break;
          case ProvPhase.idle:       break;
        }
      });
    });
    await _provisioner.provision(_ssidCtrl.text, _passCtrl.text);
    await sub.cancel();
    if (mounted) setState(() => _provisioning = false);
  }
```

Extend `dispose()` (before `super.dispose()`):

```dart
    _ssidCtrl.dispose();
    _passCtrl.dispose();
    _provisioner.dispose();
```

- [ ] **Step 2: Render the provisioning section**

In `build`, insert the provisioning section between the `const Divider(height: 1)` and the `Expanded(...)` list. Add this widget to the outer `Column`'s children, right after `const Divider(height: 1),`:

```dart
          Padding(
            padding: const EdgeInsets.fromLTRB(24, 12, 24, 8),
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                const Text('Configure device Wi-Fi',
                    style: TextStyle(fontWeight: FontWeight.bold)),
                TextField(
                  controller: _ssidCtrl,
                  decoration: const InputDecoration(labelText: 'SSID'),
                ),
                TextField(
                  controller: _passCtrl,
                  obscureText: true,
                  decoration: const InputDecoration(labelText: 'Password'),
                ),
                const SizedBox(height: 8),
                Row(
                  children: [
                    FilledButton(
                      onPressed: (_running || _provisioning) ? null : _sendWifi,
                      child: const Text('Send to device'),
                    ),
                    const SizedBox(width: 12),
                    Expanded(
                      child: Text(
                        _running ? 'Stop feeding to configure device Wi-Fi' : _provStatus,
                        style: const TextStyle(color: Colors.black54),
                        maxLines: 2,
                        overflow: TextOverflow.ellipsis,
                      ),
                    ),
                  ],
                ),
              ],
            ),
          ),
          const Divider(height: 1),
```

(The list `Expanded(...)` stays after this second divider.)

- [ ] **Step 3: Verify**

Run: `flutter test` → all pass (existing widget test still finds the title + 'Start feeding device' button; the new section doesn't break it). Then `flutter analyze` → clean.

- [ ] **Step 4: Commit**

```bash
git add companion/lib/ui/home_screen.dart
git commit -m "feat(app): home-screen Wi-Fi provisioning section (gated on feeder stopped)"
```

---

### Task 6: Full verify + on-device acceptance

**Files:** none (verification only).

- [ ] **Step 1: Native + firmware**

Run: `pio test -e native -f test_core` → PASS (incl. `test_parse_wifi_config`).
Run: `pio run -e esp32-s3` → SUCCESS.

- [ ] **Step 2: App**

Run: `cd companion && flutter test` → all pass.
Run: `flutter analyze` → clean.

- [ ] **Step 3: On-device** (manual, requires device + phone)

Flash: `pio run -e esp32-s3 -t upload`; run the app (`flutter run --release`).

Acceptance checklist:
- With the feeder **stopped**, enter SSID + password, tap **Send to device**. The
  device shows "Configuring Wi-Fi…", joins the network, and the app shows
  "Connected: <ip>".
- A wrong password shows "Failed" in the app within ~12 s.
- The provisioned network persists across a device reboot (autoConnect uses it; no
  captive portal).
- While the feeder is **running**, the Send button is disabled with the
  "Stop feeding to configure device Wi-Fi" hint.

---

## After implementation

Use superpowers:finishing-a-development-branch. This branch (`wifi-captive-portal`) now carries BOTH the captive portal (#8) and BLE provisioning — verify tests, then merge + push together. The note-only docs-drift follow-up still applies (README/ARCHITECTURE/HARDWARE/CLAUDE.md don't yet describe the recent features).
