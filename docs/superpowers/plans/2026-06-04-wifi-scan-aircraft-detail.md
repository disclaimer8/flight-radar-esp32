# Wi-Fi Network Picker + Aircraft Detail Sheet Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Device-side Wi-Fi scan exposed to the companion app as a network picker, plus a live aircraft-detail bottom sheet with photo, full data grid, and a mini-map.

**Architecture:** New BLE characteristic `f1a90004` (WRITE+NOTIFY): app writes a 3-byte scan request, firmware runs an async Wi-Fi scan from `loop()` and notifies one ≤40-byte record per network (fits any MTU). Wire format lives in a host-testable header (`src/wifi_scan_core.h`) mirrored byte-exact in Dart (`lib/packet/wifi_scan_packet.dart`), same pattern as the existing wifi-config pair. The detail sheet subscribes to the existing `GatewayController.status` stream and re-finds its aircraft by hex each update; observer position for the map is parsed from the status `fix` string ("51.5074, -0.1278") — no isolate-protocol changes.

**Tech Stack:** ESP32 Arduino/NimBLE (PlatformIO), Unity host tests, Flutter + flutter_blue_plus, new deps `flutter_map` + `latlong2` (OSM tiles, no API key).

**Spec:** `docs/superpowers/specs/2026-06-04-wifi-scan-aircraft-detail-design.md`

**Conventions that bind every task:**
- Firmware write callbacks ONLY buffer/flag; `loop()` acts (no-race rule, see `IngestCallbacks`).
- Host tests: `pio test -e native -f test_core` from the repo root (native env skips the .ino via the ARDUINO guard).
- Flutter tests: `cd companion && flutter test`.
- Commit after every green task, from the repo root.

---

### Task 1: `src/wifi_scan_core.h` — wire format (host-tested)

**Files:**
- Create: `src/wifi_scan_core.h`
- Modify: `test/test_core/test_main.cpp` (add includes, 4 test functions, 4 RUN_TEST lines)

- [ ] **Step 1: Write the failing tests**

In `test/test_core/test_main.cpp`, add `#include "wifi_scan_core.h"` next to the existing `#include "wifi_config_core.h"` (same include style as the other core headers). Add before `main`/the RUN_TEST block:

```cpp
void test_wifi_scan_request_parse() {
    uint8_t ok[] = {0x57, 0x53, 0x01};
    TEST_ASSERT_TRUE(isScanRequest(ok, 3));
    uint8_t okTrail[] = {0x57, 0x53, 0x01, 0xFF};   // trailing bytes tolerated
    TEST_ASSERT_TRUE(isScanRequest(okTrail, 4));
    uint8_t badMagic[] = {0x57, 0x43, 0x01};        // "WC" = wifi-config, not scan
    TEST_ASSERT_FALSE(isScanRequest(badMagic, 3));
    uint8_t badVer[] = {0x57, 0x53, 0x02};
    TEST_ASSERT_FALSE(isScanRequest(badVer, 3));
    TEST_ASSERT_FALSE(isScanRequest(ok, 2));        // truncated
    TEST_ASSERT_FALSE(isScanRequest(nullptr, 3));
}

void test_wifi_scan_record_encode() {
    uint8_t buf[WIFISCAN_REC_MAX];
    ScanNet n{"HomeNet", -62, true};
    size_t len = encodeScanRecord(buf, 3, 1, n);
    TEST_ASSERT_EQUAL(8 + 7, len);
    TEST_ASSERT_EQUAL_HEX8(0x57, buf[0]);                    // 'W'
    TEST_ASSERT_EQUAL_HEX8(0x4E, buf[1]);                    // 'N'
    TEST_ASSERT_EQUAL_HEX8(1, buf[2]);                       // version
    TEST_ASSERT_EQUAL_HEX8(3, buf[3]);                       // total
    TEST_ASSERT_EQUAL_HEX8(1, buf[4]);                       // index
    TEST_ASSERT_EQUAL_HEX8((uint8_t)(int8_t)-62, buf[5]);    // rssi as int8
    TEST_ASSERT_EQUAL_HEX8(1, buf[6]);                       // secured
    TEST_ASSERT_EQUAL_HEX8(7, buf[7]);                       // ssidLen
    TEST_ASSERT_EQUAL_MEMORY("HomeNet", buf + 8, 7);

    ScanNet maxSsid{std::string(32, 'a'), -50, false};
    TEST_ASSERT_EQUAL(40, encodeScanRecord(buf, 1, 0, maxSsid));
    ScanNet tooBig{std::string(33, 'a'), -50, false};
    TEST_ASSERT_EQUAL(0, encodeScanRecord(buf, 1, 0, tooBig));
    ScanNet empty{"", -50, false};
    TEST_ASSERT_EQUAL(0, encodeScanRecord(buf, 1, 0, empty));
}

void test_wifi_scan_empty_encode() {
    uint8_t buf[8];
    TEST_ASSERT_EQUAL(4, encodeScanEmpty(buf));
    TEST_ASSERT_EQUAL_HEX8(0x57, buf[0]);
    TEST_ASSERT_EQUAL_HEX8(0x4E, buf[1]);
    TEST_ASSERT_EQUAL_HEX8(1, buf[2]);
    TEST_ASSERT_EQUAL_HEX8(0, buf[3]);   // total=0 → none found
}

void test_wifi_scan_dedup_sort_cap() {
    std::vector<ScanNet> in = {
        {"B", -80, true}, {"A", -60, false}, {"B", -50, true}, {"", -10, false},
    };
    auto out = dedupSortCap(in);
    TEST_ASSERT_EQUAL(2, out.size());
    TEST_ASSERT_EQUAL_STRING("B", out[0].ssid.c_str());  // strongest duplicate kept
    TEST_ASSERT_EQUAL(-50, out[0].rssi);
    TEST_ASSERT_EQUAL_STRING("A", out[1].ssid.c_str());  // sorted by RSSI desc

    std::vector<ScanNet> many;
    for (int i = 0; i < 20; i++)
        many.push_back({"n" + std::to_string(i), (int8_t)(-30 - i), false});
    TEST_ASSERT_EQUAL(15, dedupSortCap(many).size());    // capped
}
```

Add to the RUN_TEST block (before `return UNITY_END();`):

```cpp
    RUN_TEST(test_wifi_scan_request_parse);
    RUN_TEST(test_wifi_scan_record_encode);
    RUN_TEST(test_wifi_scan_empty_encode);
    RUN_TEST(test_wifi_scan_dedup_sort_cap);
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `pio test -e native -f test_core`
Expected: compile error — `wifi_scan_core.h: No such file or directory`

- [ ] **Step 3: Write the header**

Create `src/wifi_scan_core.h`:

```cpp
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

// Wi-Fi scan-on-demand wire format (BLE characteristic f1a90004). The app
// writes a scan request; the device replies with one notify per network so
// every notify fits any negotiated MTU (iOS can be as low as 185).
constexpr uint8_t WIFISCAN_MAGIC0       = 0x57; // 'W'
constexpr uint8_t WIFISCAN_REQ_MAGIC1   = 0x53; // 'S' (request, app -> device)
constexpr uint8_t WIFISCAN_REC_MAGIC1   = 0x4E; // 'N' (record, device -> app)
constexpr uint8_t WIFISCAN_VERSION      = 1;
constexpr size_t  WIFISCAN_MAX_SSID     = 32;
constexpr size_t  WIFISCAN_MAX_NETWORKS = 15;
constexpr size_t  WIFISCAN_REC_MAX      = 8 + WIFISCAN_MAX_SSID; // 40 B

// Plain aggregate (no member initializers) so C++11 brace-init works in both
// the native test env and the ESP32 toolchain; producers set every field.
struct ScanNet {
    std::string ssid;
    int8_t rssi;
    bool secured;
};

// "WS" + ver. Trailing bytes are tolerated (future use).
inline bool isScanRequest(const uint8_t* buf, size_t len) {
    return buf && len >= 3 &&
           buf[0] == WIFISCAN_MAGIC0 && buf[1] == WIFISCAN_REQ_MAGIC1 &&
           buf[2] == WIFISCAN_VERSION;
}

// Drop hidden (empty-SSID) and oversize-SSID networks, dedup by SSID keeping
// the strongest RSSI, sort by RSSI descending, cap at WIFISCAN_MAX_NETWORKS.
inline std::vector<ScanNet> dedupSortCap(const std::vector<ScanNet>& in) {
    std::vector<ScanNet> out;
    for (const auto& n : in) {
        if (n.ssid.empty() || n.ssid.size() > WIFISCAN_MAX_SSID) continue;
        bool merged = false;
        for (auto& o : out) {
            if (o.ssid == n.ssid) {
                if (n.rssi > o.rssi) { o.rssi = n.rssi; o.secured = n.secured; }
                merged = true;
                break;
            }
        }
        if (!merged) out.push_back(n);
    }
    std::sort(out.begin(), out.end(),
              [](const ScanNet& a, const ScanNet& b) { return a.rssi > b.rssi; });
    if (out.size() > WIFISCAN_MAX_NETWORKS) out.resize(WIFISCAN_MAX_NETWORKS);
    return out;
}

// "WN" + ver + total + index + rssi(int8) + secured + ssidLen + ssid.
// Returns bytes written, 0 if the record is invalid. buf must hold
// WIFISCAN_REC_MAX bytes.
inline size_t encodeScanRecord(uint8_t* buf, uint8_t total, uint8_t index,
                               const ScanNet& n) {
    if (!buf || n.ssid.empty() || n.ssid.size() > WIFISCAN_MAX_SSID) return 0;
    buf[0] = WIFISCAN_MAGIC0;
    buf[1] = WIFISCAN_REC_MAGIC1;
    buf[2] = WIFISCAN_VERSION;
    buf[3] = total;
    buf[4] = index;
    buf[5] = static_cast<uint8_t>(n.rssi);
    buf[6] = n.secured ? 1 : 0;
    buf[7] = static_cast<uint8_t>(n.ssid.size());
    std::memcpy(buf + 8, n.ssid.data(), n.ssid.size());
    return 8 + n.ssid.size();
}

// 4-byte "no networks found" notify: "WN" + ver + total=0.
inline size_t encodeScanEmpty(uint8_t* buf) {
    buf[0] = WIFISCAN_MAGIC0;
    buf[1] = WIFISCAN_REC_MAGIC1;
    buf[2] = WIFISCAN_VERSION;
    buf[3] = 0;
    return 4;
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `pio test -e native -f test_core`
Expected: all cases PASS (52 existing + 4 new = 56)

- [ ] **Step 5: Commit**

```bash
git add src/wifi_scan_core.h test/test_core/test_main.cpp
git commit -m "feat(firmware): wifi scan wire format (wifi_scan_core.h, host-tested)"
```

---

### Task 2: Firmware — `f1a90004` characteristic + async scan in `loop()`

**Files:**
- Modify: `src/flight_ticker.ino` (BLE constants ~line 64, callbacks ~line 100, helper near `notifyWifiStatus` ~line 449, `setup()` GATT block ~line 507, `loop()` ~line 534)

No host test possible (Arduino-only code); verification is a clean device build. The pure logic was tested in Task 1.

- [ ] **Step 1: Add include, constants, state, and callback**

Next to the other `#include "..."` core headers at the top of `flight_ticker.ino`:

```cpp
#include "wifi_scan_core.h"
```

Next to `BLE_WIFICFG_UUID` (~line 67):

```cpp
static const char* BLE_WIFISCAN_UUID = "f1a90004-7e1d-4c2a-9b3f-1a2b3c4d5e6f";
```

After the `WifiConfigCallbacks` block (~line 100):

```cpp
NimBLECharacteristic* g_wifiScanChar = nullptr;
volatile bool g_wifiScanRequested = false;
bool g_wifiScanInFlight = false;   // loop()-only; not shared with BLE task

// Scan-request write: like the other callbacks, only set a flag; loop() runs
// the (async) scan and notifies results off the BLE task.
class WifiScanCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c) override {
        std::string v = c->getValue();
        if (isScanRequest(reinterpret_cast<const uint8_t*>(v.data()), v.size()))
            g_wifiScanRequested = true;
    }
};
```

- [ ] **Step 2: Add the notify helper**

After `notifyWifiStatus` (~line 459):

```cpp
// Send scan results to the app: one notify per network (each fits any MTU),
// or a single total=0 notify when nothing was found / the scan failed.
void sendScanResults(const std::vector<ScanNet>& nets) {
    if (!g_wifiScanChar) return;
    uint8_t buf[WIFISCAN_REC_MAX];
    if (nets.empty()) {
        g_wifiScanChar->setValue(buf, encodeScanEmpty(buf));
        g_wifiScanChar->notify();
        return;
    }
    uint8_t total = static_cast<uint8_t>(nets.size());
    for (uint8_t i = 0; i < total; i++) {
        size_t len = encodeScanRecord(buf, total, i, nets[i]);
        if (!len) continue;
        g_wifiScanChar->setValue(buf, len);
        g_wifiScanChar->notify();
        delay(20);   // pace notifies so the central's queue keeps up
    }
}
```

- [ ] **Step 3: Register the characteristic in `setup()`**

After the `g_wifiCfgChar` block (~line 517):

```cpp
    g_wifiScanChar = bleSvc->createCharacteristic(
        BLE_WIFISCAN_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::NOTIFY);
    g_wifiScanChar->setCallbacks(new WifiScanCallbacks());
```

- [ ] **Step 4: Drive the scan from `loop()`**

After the `g_wifiCfgReady` block in `loop()` (~line 550):

```cpp
    if (g_wifiScanRequested && !g_wifiScanInFlight) {
        g_wifiScanRequested = false;
        WiFi.scanNetworks(/*async=*/true);   // blocking scan would freeze the radar 2-3 s
        g_wifiScanInFlight = true;
    }
    if (g_wifiScanInFlight) {
        int n = WiFi.scanComplete();
        if (n >= 0) {
            std::vector<ScanNet> nets;
            for (int i = 0; i < n; i++) {
                ScanNet s;
                s.ssid = std::string(WiFi.SSID(i).c_str());
                int rssi = WiFi.RSSI(i);
                s.rssi = static_cast<int8_t>(rssi < -128 ? -128 : (rssi > 127 ? 127 : rssi));
                s.secured = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
                nets.push_back(s);
            }
            WiFi.scanDelete();
            sendScanResults(dedupSortCap(nets));
            g_wifiScanInFlight = false;
        } else if (n == WIFI_SCAN_FAILED) {
            sendScanResults({});
            g_wifiScanInFlight = false;
        }
        // n == WIFI_SCAN_RUNNING (-1): keep waiting
    }
```

(A new request while one is in flight is ignored by the `!g_wifiScanInFlight` guard; `g_wifiScanRequested` is consumed so it doesn't fire twice.)

- [ ] **Step 5: Verify both builds**

Run: `pio test -e native -f test_core`
Expected: 56 PASS (host build unaffected)

Run: `pio run -e esp32-s3`
Expected: `SUCCESS` (firmware compiles + links)

- [ ] **Step 6: Commit**

```bash
git add src/flight_ticker.ino
git commit -m "feat(firmware): f1a90004 wifi-scan characteristic + async scan from loop()"
```

---

### Task 3: Dart wire mirror — `wifi_scan_packet.dart` + `ScanCollector`

**Files:**
- Create: `companion/lib/packet/wifi_scan_packet.dart`
- Create: `companion/test/wifi_scan_packet_test.dart`

- [ ] **Step 1: Write the failing tests**

Create `companion/test/wifi_scan_packet_test.dart`:

```dart
import 'package:flutter_test/flutter_test.dart';
import 'package:flight_radar_companion/packet/wifi_scan_packet.dart';

List<int> rec(int total, int index, String ssid, int rssi,
        {bool secured = false}) =>
    [0x57, 0x4E, 1, total, index, rssi & 0xFF, secured ? 1 : 0,
     ssid.length, ...ssid.codeUnits];

void main() {
  test('encodeScanRequest emits WS v1', () {
    expect(encodeScanRequest(), [0x57, 0x53, 1]);
  });

  test('parses a record notify', () {
    final n = parseScanNotify(rec(3, 1, 'HomeNet', -90, secured: true))!;
    expect(n.total, 3);
    expect(n.index, 1);
    expect(n.net!.ssid, 'HomeNet');
    expect(n.net!.rssi, -90);
    expect(n.net!.secured, isTrue);
  });

  test('parses the 4-byte empty notify', () {
    final n = parseScanNotify([0x57, 0x4E, 1, 0])!;
    expect(n.total, 0);
    expect(n.index, isNull);
    expect(n.net, isNull);
  });

  test('rejects malformed notifies', () {
    expect(parseScanNotify([]), isNull);
    expect(parseScanNotify([0x57, 0x4E, 1]), isNull);                  // short header
    expect(parseScanNotify([0x57, 0x43, 1, 1, 0, 0, 0, 1, 65]), isNull); // wrong magic
    expect(parseScanNotify([0x57, 0x4E, 2, 1, 0, 0, 0, 1, 65]), isNull); // wrong version
    expect(parseScanNotify([0x57, 0x4E, 1, 1, 0, 0, 0, 5, 65]), isNull); // truncated ssid
    expect(parseScanNotify([0x57, 0x4E, 1, 1, 0, 0, 0, 0]), isNull);     // zero ssidLen
  });

  test('collector completes when all records arrive, in any order', () {
    final c = ScanCollector();
    expect(c.add(rec(2, 1, 'B', -80)), isFalse);
    expect(c.add(rec(2, 1, 'B', -80)), isFalse); // duplicate index ignored
    expect(c.add([0, 1, 2]), isFalse);           // malformed ignored
    expect(c.add(rec(2, 0, 'A', -60)), isTrue);
    expect(c.networks.map((n) => n.ssid).toList(), ['A', 'B']);
  });

  test('collector completes immediately on the empty notify', () {
    final c = ScanCollector();
    expect(c.add([0x57, 0x4E, 1, 0]), isTrue);
    expect(c.networks, isEmpty);
  });
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd companion && flutter test test/wifi_scan_packet_test.dart`
Expected: FAIL — `Error: Couldn't resolve the package ... wifi_scan_packet.dart`

- [ ] **Step 3: Write the implementation**

Create `companion/lib/packet/wifi_scan_packet.dart`:

```dart
import 'dart:convert';
import 'dart:typed_data';

// Mirror of src/wifi_scan_core.h (byte-exact).
const int wifiScanMagic0 = 0x57; // 'W'
const int wifiScanReqMagic1 = 0x53; // 'S' (request)
const int wifiScanRecMagic1 = 0x4E; // 'N' (record)
const int wifiScanVersion = 1;
const int wifiScanMaxSsid = 32;

/// One network the device can see.
class WifiNetwork {
  final String ssid;
  final int rssi; // dBm, negative
  final bool secured;
  const WifiNetwork(this.ssid, this.rssi, this.secured);
}

/// One parsed notify. [index]/[net] are null for the 4-byte "none found"
/// notify (total = 0).
class ScanNotify {
  final int total;
  final int? index;
  final WifiNetwork? net;
  const ScanNotify(this.total, [this.index, this.net]);
}

/// Build the WRITE packet: "WS" + ver.
Uint8List encodeScanRequest() =>
    Uint8List.fromList([wifiScanMagic0, wifiScanReqMagic1, wifiScanVersion]);

/// Parse one NOTIFY. Returns null on malformed bytes.
ScanNotify? parseScanNotify(List<int> bytes) {
  if (bytes.length < 4) return null;
  if (bytes[0] != wifiScanMagic0 || bytes[1] != wifiScanRecMagic1) return null;
  if (bytes[2] != wifiScanVersion) return null;
  final total = bytes[3];
  if (total == 0) return const ScanNotify(0);
  if (bytes.length < 8) return null;
  final index = bytes[4];
  final rssi = bytes[5].toSigned(8);
  final secured = bytes[6] != 0;
  final ssidLen = bytes[7];
  if (ssidLen == 0 || ssidLen > wifiScanMaxSsid) return null;
  if (bytes.length < 8 + ssidLen) return null;
  final ssid = utf8.decode(bytes.sublist(8, 8 + ssidLen), allowMalformed: true);
  return ScanNotify(total, index, WifiNetwork(ssid, rssi, secured));
}

/// Accumulates scan notifies until all [ScanNotify.total] records arrived.
/// Index-keyed, so duplicate or out-of-order notifies are harmless.
class ScanCollector {
  int? _total;
  final _byIndex = <int, WifiNetwork>{};

  /// Feed one notify; returns true when the result set is complete.
  bool add(List<int> bytes) {
    final n = parseScanNotify(bytes);
    if (n == null) return false;
    if (n.total == 0) {
      _total = 0;
      return true;
    }
    _total = n.total;
    if (n.index != null && n.net != null) _byIndex[n.index!] = n.net!;
    return _byIndex.length >= _total!;
  }

  /// Collected networks in index order (device already sorted by RSSI).
  List<WifiNetwork> get networks {
    final keys = _byIndex.keys.toList()..sort();
    return [for (final k in keys) _byIndex[k]!];
  }
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd companion && flutter test test/wifi_scan_packet_test.dart`
Expected: 6 tests PASS

- [ ] **Step 5: Commit**

```bash
git add companion/lib/packet/wifi_scan_packet.dart companion/test/wifi_scan_packet_test.dart
git commit -m "feat(companion): wifi-scan wire mirror + notify collector"
```

---

### Task 4: Shared device finder + `WifiScanner`

**Files:**
- Create: `companion/lib/ble/device_finder.dart`
- Create: `companion/lib/ble/wifi_scanner.dart`
- Modify: `companion/lib/ble/wifi_provisioner.dart` (use the shared finder)

The session logic (connect → subscribe → write → collect) is thin plumbing over the Task-3 collector; like `WifiProvisioner`, it has no unit test — the pure parts are already covered.

- [ ] **Step 1: Extract the device finder**

Create `companion/lib/ble/device_finder.dart` (body lifted verbatim from `WifiProvisioner._scanForDevice`):

```dart
import 'dart:async';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';

final Guid radarServiceUuid = Guid('f1a90001-7e1d-4c2a-9b3f-1a2b3c4d5e6f');

/// BLE-scan for the FlightRadar device by service UUID. Returns null when the
/// adapter is off or nothing is found within the timeout.
Future<BluetoothDevice?> findRadarDevice() async {
  final on = await FlutterBluePlus.adapterState
      .where((s) => s == BluetoothAdapterState.on)
      .first
      .timeout(const Duration(seconds: 5),
          onTimeout: () => BluetoothAdapterState.off);
  if (on != BluetoothAdapterState.on) return null;
  final completer = Completer<BluetoothDevice?>();
  final sub = FlutterBluePlus.onScanResults.listen((results) {
    if (results.isNotEmpty && !completer.isCompleted) {
      completer.complete(results.first.device);
    }
  });
  await FlutterBluePlus.startScan(
      withServices: [radarServiceUuid], timeout: const Duration(seconds: 15));
  final device = await completer.future
      .timeout(const Duration(seconds: 16), onTimeout: () => null);
  await sub.cancel();
  await FlutterBluePlus.stopScan();
  return device;
}
```

In `companion/lib/ble/wifi_provisioner.dart`:
- add `import 'device_finder.dart';`
- delete the whole `_scanForDevice` method (lines 78–94)
- replace `device = await _scanForDevice();` with `device = await findRadarDevice();`
- `serviceUuid` static stays (it's used in the characteristic walk).

- [ ] **Step 2: Write `WifiScanner`**

Create `companion/lib/ble/wifi_scanner.dart`:

```dart
import 'dart:async';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import '../packet/wifi_scan_packet.dart';
import 'device_finder.dart';

class WifiScanException implements Exception {
  final String message;
  const WifiScanException(this.message);
  @override
  String toString() => message;
}

/// On-demand "which networks can the device see" scan, mirroring
/// WifiProvisioner's connect flow: find device, subscribe to the scan
/// characteristic, write a request, collect record notifies. The device is a
/// single-central peripheral, so the feeder must be stopped before calling.
class WifiScanner {
  static final Guid wifiScanUuid = Guid('f1a90004-7e1d-4c2a-9b3f-1a2b3c4d5e6f');

  /// Networks visible to the device, strongest first. Throws
  /// [WifiScanException] with a user-facing reason on any failure.
  Future<List<WifiNetwork>> scan() async {
    BluetoothDevice? device;
    StreamSubscription<List<int>>? notifySub;
    try {
      device = await findRadarDevice();
      if (device == null) throw const WifiScanException('device not found');
      await device.connect(
          license: License.nonprofit, timeout: const Duration(seconds: 15));
      final services = await device.discoverServices();
      BluetoothCharacteristic? ch;
      for (final s in services) {
        if (s.uuid == radarServiceUuid) {
          for (final c in s.characteristics) {
            if (c.uuid == wifiScanUuid) ch = c;
          }
        }
      }
      if (ch == null) {
        throw const WifiScanException('characteristic missing (old firmware?)');
      }

      final collector = ScanCollector();
      final done = Completer<void>();
      await ch.setNotifyValue(true);
      notifySub = ch.onValueReceived.listen((bytes) {
        if (collector.add(bytes) && !done.isCompleted) done.complete();
      });
      await ch.write(encodeScanRequest(), withoutResponse: false);
      await done.future.timeout(const Duration(seconds: 15), onTimeout: () {
        throw const WifiScanException('scan timeout');
      });
      return collector.networks;
    } on WifiScanException {
      rethrow;
    } catch (e) {
      throw WifiScanException(e.toString());
    } finally {
      await notifySub?.cancel();
      try {
        await device?.disconnect();
      } catch (_) {}
    }
  }
}
```

- [ ] **Step 3: Verify the suite still passes and the analyzer is clean**

Run: `cd companion && flutter analyze lib/ble && flutter test`
Expected: no analyzer issues; all tests PASS (provisioner refactor changed no behavior)

- [ ] **Step 4: Commit**

```bash
git add companion/lib/ble/device_finder.dart companion/lib/ble/wifi_scanner.dart companion/lib/ble/wifi_provisioner.dart
git commit -m "feat(companion): WifiScanner + shared findRadarDevice (DRY with provisioner)"
```

---

### Task 5: `NetworkPicker` widget

**Files:**
- Create: `companion/lib/ui/network_picker.dart`
- Create: `companion/test/network_picker_test.dart`

- [ ] **Step 1: Write the failing test**

Create `companion/test/network_picker_test.dart`:

```dart
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:flight_radar_companion/packet/wifi_scan_packet.dart';
import 'package:flight_radar_companion/ui/network_picker.dart';

void main() {
  testWidgets('lists networks with lock icons and returns the tapped one',
      (tester) async {
    WifiNetwork? picked;
    await tester.pumpWidget(MaterialApp(
      home: Builder(
        builder: (context) => ElevatedButton(
          onPressed: () async {
            picked = await showModalBottomSheet<WifiNetwork>(
              context: context,
              builder: (_) => const NetworkPicker(networks: [
                WifiNetwork('HomeNet', -55, true),
                WifiNetwork('CoffeeShop', -82, false),
              ]),
            );
          },
          child: const Text('open'),
        ),
      ),
    ));
    await tester.tap(find.text('open'));
    await tester.pumpAndSettle();

    expect(find.text('HomeNet'), findsOneWidget);
    expect(find.text('CoffeeShop'), findsOneWidget);
    expect(find.byIcon(Icons.lock_outline), findsOneWidget); // only HomeNet is secured

    await tester.tap(find.text('HomeNet'));
    await tester.pumpAndSettle();
    expect(picked, isNotNull);
    expect(picked!.ssid, 'HomeNet');
  });
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd companion && flutter test test/network_picker_test.dart`
Expected: FAIL — `network_picker.dart` doesn't exist

- [ ] **Step 3: Write the widget**

Create `companion/lib/ui/network_picker.dart`:

```dart
import 'package:flutter/material.dart';
import '../packet/wifi_scan_packet.dart';

/// Bottom-sheet list of networks the device reported. Pops with the tapped
/// [WifiNetwork] (or null when dismissed).
class NetworkPicker extends StatelessWidget {
  final List<WifiNetwork> networks;
  const NetworkPicker({super.key, required this.networks});

  IconData _signalIcon(int rssi) {
    if (rssi >= -60) return Icons.wifi;
    if (rssi >= -75) return Icons.wifi_2_bar;
    return Icons.wifi_1_bar;
  }

  @override
  Widget build(BuildContext context) {
    return SafeArea(
      child: Column(
        mainAxisSize: MainAxisSize.min,
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          const Padding(
            padding: EdgeInsets.fromLTRB(24, 16, 24, 4),
            child: Text('Networks the device can see',
                style: TextStyle(fontWeight: FontWeight.bold)),
          ),
          Flexible(
            child: ListView.builder(
              shrinkWrap: true,
              itemCount: networks.length,
              itemBuilder: (context, i) {
                final n = networks[i];
                return ListTile(
                  leading: Icon(_signalIcon(n.rssi)),
                  title: Text(n.ssid, maxLines: 1, overflow: TextOverflow.ellipsis),
                  subtitle: Text('${n.rssi} dBm'),
                  trailing: n.secured ? const Icon(Icons.lock_outline) : null,
                  onTap: () => Navigator.pop(context, n),
                );
              },
            ),
          ),
        ],
      ),
    );
  }
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd companion && flutter test test/network_picker_test.dart`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add companion/lib/ui/network_picker.dart companion/test/network_picker_test.dart
git commit -m "feat(companion): network picker bottom sheet"
```

---

### Task 6: Scan button in the Wi-Fi section of `home_screen.dart`

**Files:**
- Modify: `companion/lib/ui/home_screen.dart`
- Modify: `companion/test/widget_test.dart` (button presence check)

- [ ] **Step 1: Write the failing test**

Add to `companion/test/widget_test.dart` inside `main()`:

```dart
  testWidgets('wifi section has a scan-networks button', (tester) async {
    await tester.pumpWidget(const CompanionApp());
    expect(find.byIcon(Icons.wifi_find), findsOneWidget);
  });
```

Run: `cd companion && flutter test test/widget_test.dart`
Expected: new test FAILS (icon not found)

- [ ] **Step 2: Wire the scan flow into `_HomeScreenState`**

In `companion/lib/ui/home_screen.dart`:

Add imports:

```dart
import '../ble/wifi_scanner.dart';
import '../packet/wifi_scan_packet.dart';
import 'network_picker.dart';
```

Add state fields next to `_provisioning` (line 28):

```dart
  bool _scanning = false;
  final _passFocus = FocusNode();
```

Dispose `_passFocus` in `dispose()` (next to the controllers):

```dart
    _passFocus.dispose();
```

Add the handler after `_sendWifi` (line 136):

```dart
  Future<void> _scanWifi() async {
    if (_running || _provisioning || _scanning) return;
    if (!await _requestBlePermissions()) {
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(const SnackBar(
          content: Text('Bluetooth permission is required to scan'),
        ));
      }
      return;
    }
    setState(() { _scanning = true; _provStatus = 'Scanning networks…'; });
    try {
      final nets = await WifiScanner().scan();
      if (!mounted) return;
      setState(() => _provStatus = nets.isEmpty ? 'No networks found' : '');
      if (nets.isEmpty) return;
      final picked = await showModalBottomSheet<WifiNetwork>(
        context: context,
        builder: (_) => NetworkPicker(networks: nets),
      );
      if (picked != null && mounted) {
        _ssidCtrl.text = picked.ssid;
        _passFocus.requestFocus();
      }
    } on WifiScanException catch (e) {
      if (mounted) setState(() => _provStatus = 'Scan failed: $e');
    } finally {
      if (mounted) setState(() => _scanning = false);
    }
  }
```

- [ ] **Step 3: Replace the SSID TextField with field + button row**

In `build()`, replace:

```dart
                  TextField(
                    controller: _ssidCtrl,
                    decoration: const InputDecoration(labelText: 'SSID'),
                  ),
```

with:

```dart
                  Row(
                    crossAxisAlignment: CrossAxisAlignment.end,
                    children: [
                      Expanded(
                        child: TextField(
                          controller: _ssidCtrl,
                          decoration: const InputDecoration(labelText: 'SSID'),
                        ),
                      ),
                      IconButton(
                        tooltip: 'Scan networks via device',
                        onPressed:
                            (_running || _provisioning || _scanning) ? null : _scanWifi,
                        icon: _scanning
                            ? const SizedBox(
                                width: 20, height: 20,
                                child: CircularProgressIndicator(strokeWidth: 2))
                            : const Icon(Icons.wifi_find),
                      ),
                    ],
                  ),
```

and add the focus node to the password field:

```dart
                  TextField(
                    controller: _passCtrl,
                    focusNode: _passFocus,
                    obscureText: true,
                    decoration: const InputDecoration(labelText: 'Password'),
                  ),
```

- [ ] **Step 4: Run the suite**

Run: `cd companion && flutter test`
Expected: all PASS (including the new button test and the landscape-overflow test)

- [ ] **Step 5: Commit**

```bash
git add companion/lib/ui/home_screen.dart companion/test/widget_test.dart
git commit -m "feat(companion): scan-networks button + picker flow in wifi section"
```

---

### Task 7: Map dependencies

**Files:**
- Modify: `companion/pubspec.yaml`, `companion/pubspec.lock` (via pub)

- [ ] **Step 1: Add deps**

Run: `cd companion && flutter pub add flutter_map latlong2`
Expected: `Changed N dependencies!` — flutter_map resolves to the current major (8.x); both are pure-Dart/Flutter packages (no platform plugin code, no pod changes).

- [ ] **Step 2: Verify the suite still passes**

Run: `cd companion && flutter test`
Expected: all PASS

- [ ] **Step 3: Commit**

```bash
git add companion/pubspec.yaml companion/pubspec.lock
git commit -m "chore(companion): add flutter_map + latlong2 for the detail mini-map"
```

---

### Task 8: `AircraftDetailSheet`

**Files:**
- Create: `companion/lib/ui/aircraft_detail_sheet.dart`
- Create: `companion/test/aircraft_detail_sheet_test.dart`

The map itself is excluded from widget tests (`showMap: false`) — tile fetching needs the network; the map gets verified on hardware in Task 10. Everything else (fields, fallbacks, live updates, fix parsing) is tested.

- [ ] **Step 1: Write the failing tests**

Create `companion/test/aircraft_detail_sheet_test.dart`:

```dart
import 'dart:async';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:http/testing.dart';
import 'package:http/http.dart' as http;
import 'package:flight_radar_companion/data/aircraft.dart';
import 'package:flight_radar_companion/data/photo_client.dart';
import 'package:flight_radar_companion/service/gateway_engine.dart'
    show GatewayStatus;
import 'package:flight_radar_companion/ui/aircraft_detail_sheet.dart';

PhotoClient noPhotos() =>
    PhotoClient(MockClient((_) async => http.Response('{"photos":[]}', 200)));

const full = Aircraft(
  callsign: 'BAW117', type: 'A388', lat: 51.5, lon: -0.45,
  altFt: 35000, gsKt: 450, onGround: false, track: 270, squawk: 7700,
  registration: 'G-XLEA', origin: 'EGLL', dest: 'KJFK',
  hex: '40612a', desc: 'Airbus A380-841', isEmergency: true, distKm: 8.3,
);

const sparse = Aircraft(
  callsign: '', type: 'GLID', lat: 50.0, lon: 14.0,
  altFt: null, gsKt: null, onGround: true,
);

Widget host(Aircraft a, Stream<GatewayStatus> status) => MaterialApp(
      home: Scaffold(
        body: AircraftDetailSheet(
            aircraft: a, photos: noPhotos(), status: status, showMap: false),
      ),
    );

void main() {
  testWidgets('renders every populated field', (tester) async {
    final status = StreamController<GatewayStatus>.broadcast();
    await tester.pumpWidget(host(full, status.stream));
    await tester.pump();

    expect(find.text('BAW117'), findsOneWidget);
    expect(find.text('Airbus A380-841'), findsOneWidget);
    expect(find.text('EMG'), findsOneWidget);
    expect(find.text('35000 ft'), findsOneWidget);
    expect(find.text('450 kt'), findsOneWidget);
    expect(find.text('270°'), findsOneWidget);
    expect(find.text('7700'), findsOneWidget);
    expect(find.text('EGLL → KJFK'), findsOneWidget);
    expect(find.text('8.3 km'), findsOneWidget);
    expect(find.text('G-XLEA'), findsOneWidget);
    expect(find.text('40612a'), findsOneWidget);
    expect(find.text('51.5000, -0.4500'), findsOneWidget);
    await status.close();
  });

  testWidgets('missing values render as a dash', (tester) async {
    final status = StreamController<GatewayStatus>.broadcast();
    await tester.pumpWidget(host(sparse, status.stream));
    await tester.pump();

    expect(find.text('——'), findsNothing);   // sanity: dashes are single
    expect(find.text('—'), findsWidgets);    // alt, speed, track, squawk, route…
    expect(find.text('Yes'), findsOneWidget); // on ground
    await status.close();
  });

  testWidgets('live update replaces data; disappearance shows signal lost',
      (tester) async {
    final status = StreamController<GatewayStatus>.broadcast();
    await tester.pumpWidget(host(full, status.stream));
    await tester.pump();

    // Same hex, new altitude → field updates.
    status.add(GatewayStatus(
        aircraft: [full.copyWith(distKm: 9.9)], fix: '51.0000, -0.5000'));
    await tester.pump();
    expect(find.text('9.9 km'), findsOneWidget);
    expect(find.textContaining('Signal lost'), findsNothing);

    // Aircraft gone from the feed → banner, last data retained.
    status.add(const GatewayStatus(aircraft: []));
    await tester.pump();
    expect(find.textContaining('Signal lost'), findsOneWidget);
    expect(find.text('BAW117'), findsOneWidget);
    await status.close();
  });

  test('parseFix extracts observer coordinates', () {
    expect(parseFix('51.5074, -0.1278')!.latitude, closeTo(51.5074, 1e-6));
    expect(parseFix('51.5074, -0.1278')!.longitude, closeTo(-0.1278, 1e-6));
    expect(parseFix('no fix'), isNull);
    expect(parseFix(''), isNull);
  });
}
```

Note: `Aircraft.copyWith` only supports origin/dest/distKm (`lib/data/aircraft.dart:44`) — `full.copyWith(distKm: 9.9)` is within that.

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd companion && flutter test test/aircraft_detail_sheet_test.dart`
Expected: FAIL — `aircraft_detail_sheet.dart` doesn't exist

- [ ] **Step 3: Write the sheet**

Create `companion/lib/ui/aircraft_detail_sheet.dart`:

```dart
import 'dart:async';
import 'dart:math' as math;
import 'package:flutter/material.dart';
import 'package:flutter_map/flutter_map.dart';
import 'package:latlong2/latlong.dart';
import '../data/aircraft.dart';
import '../data/photo_client.dart';
import '../service/gateway_engine.dart' show GatewayStatus;

/// Parse the observer position out of GatewayStatus.fix ("51.5074, -0.1278").
/// Returns null for "no fix" or anything unparseable.
LatLng? parseFix(String fix) {
  final parts = fix.split(',');
  if (parts.length != 2) return null;
  final lat = double.tryParse(parts[0].trim());
  final lon = double.tryParse(parts[1].trim());
  if (lat == null || lon == null) return null;
  return LatLng(lat, lon);
}

/// Open the live detail sheet for [aircraft].
Future<void> showAircraftDetail(BuildContext context, Aircraft aircraft,
    PhotoClient photos, Stream<GatewayStatus> status) {
  return showModalBottomSheet(
    context: context,
    isScrollControlled: true,
    builder: (_) => DraggableScrollableSheet(
      expand: false,
      initialChildSize: 0.85,
      minChildSize: 0.4,
      maxChildSize: 0.95,
      builder: (context, scroll) => AircraftDetailSheet(
        aircraft: aircraft,
        photos: photos,
        status: status,
        scrollController: scroll,
      ),
    ),
  );
}

/// Live aircraft details: photo, badges, full field grid, mini-map. Subscribes
/// to the gateway status stream and re-finds its aircraft by hex on every
/// update; when the aircraft drops out of the feed it keeps the last data and
/// shows a "Signal lost" banner.
class AircraftDetailSheet extends StatefulWidget {
  final Aircraft aircraft;
  final PhotoClient photos;
  final Stream<GatewayStatus> status;
  final ScrollController? scrollController;
  final bool showMap; // false in widget tests (tiles need the network)
  const AircraftDetailSheet({
    super.key,
    required this.aircraft,
    required this.photos,
    required this.status,
    this.scrollController,
    this.showMap = true,
  });

  @override
  State<AircraftDetailSheet> createState() => _AircraftDetailSheetState();
}

class _AircraftDetailSheetState extends State<AircraftDetailSheet> {
  late Aircraft _a = widget.aircraft;
  LatLng? _observer;
  bool _lost = false;
  StreamSubscription<GatewayStatus>? _sub;
  late final Future<PhotoRef?> _photo = widget.photos
      .lookup(reg: widget.aircraft.registration ?? '', hex: widget.aircraft.hex);

  @override
  void initState() {
    super.initState();
    _sub = widget.status.listen((s) {
      Aircraft? match;
      for (final x in s.aircraft) {
        final byHex = widget.aircraft.hex.isNotEmpty && x.hex == widget.aircraft.hex;
        final byCallsign = widget.aircraft.hex.isEmpty &&
            x.callsign == widget.aircraft.callsign;
        if (byHex || byCallsign) {
          match = x;
          break;
        }
      }
      if (!mounted) return;
      setState(() {
        _observer = parseFix(s.fix) ?? _observer;
        if (match != null) {
          _a = match;
          _lost = false;
        } else {
          _lost = true;
        }
      });
    });
  }

  @override
  void dispose() {
    _sub?.cancel();
    super.dispose();
  }

  String get _position =>
      '${_a.lat.toStringAsFixed(4)}, ${_a.lon.toStringAsFixed(4)}';

  @override
  Widget build(BuildContext context) {
    final cs = _a.callsign.isEmpty ? '------' : _a.callsign;
    final reg = _a.registration ?? '';
    final hasRoute = (_a.origin ?? '').isNotEmpty &&
        (_a.dest ?? '').isNotEmpty &&
        _a.origin != _a.dest;

    return ListView(
      controller: widget.scrollController,
      padding: const EdgeInsets.fromLTRB(20, 12, 20, 24),
      children: [
        Center(
          child: Container(
            width: 36, height: 4,
            margin: const EdgeInsets.only(bottom: 12),
            decoration: BoxDecoration(
                color: Colors.black26, borderRadius: BorderRadius.circular(2)),
          ),
        ),
        _Photo(photo: _photo),
        const SizedBox(height: 12),
        Row(children: [
          Flexible(
            child: Text(cs,
                maxLines: 1, overflow: TextOverflow.ellipsis,
                style: const TextStyle(fontWeight: FontWeight.bold, fontSize: 22)),
          ),
          const SizedBox(width: 8),
          if (_a.isEmergency) _badge('EMG', Colors.red),
          if (_a.isMilitary) _badge('MIL', Colors.green.shade700),
        ]),
        if (_a.desc.isNotEmpty || _a.type.isNotEmpty)
          Text(_a.desc.isNotEmpty ? _a.desc : _a.type,
              style: const TextStyle(color: Colors.black54)),
        if (_lost)
          Container(
            margin: const EdgeInsets.only(top: 8),
            padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 6),
            decoration: BoxDecoration(
                color: Colors.orange.shade100,
                borderRadius: BorderRadius.circular(6)),
            child: const Text('Signal lost — showing last known data'),
          ),
        const SizedBox(height: 12),
        _field('Altitude', _a.altFt == null ? '—' : '${_a.altFt} ft'),
        _field('Ground speed', _a.gsKt == null ? '—' : '${_a.gsKt} kt'),
        _field('Track', _a.track == null ? '—' : '${_a.track!.round()}°'),
        _field('Squawk',
            _a.squawk == null ? '—' : _a.squawk!.toString().padLeft(4, '0')),
        _field('Route', hasRoute ? '${_a.origin} → ${_a.dest}' : '—'),
        _field('Distance',
            _a.distKm == null ? '—' : '${_a.distKm!.toStringAsFixed(1)} km'),
        _field('Registration', reg.isEmpty ? '—' : reg),
        _field('ICAO24', _a.hex.isEmpty ? '—' : _a.hex),
        _field('Position', _position),
        _field('On ground', _a.onGround ? 'Yes' : 'No'),
        if (widget.showMap) ...[
          const SizedBox(height: 12),
          ClipRRect(
            borderRadius: BorderRadius.circular(8),
            child: SizedBox(
              height: 220,
              child: FlutterMap(
                options: MapOptions(
                  initialCenter: LatLng(_a.lat, _a.lon),
                  initialZoom: 9,
                  interactionOptions:
                      const InteractionOptions(flags: InteractiveFlag.none),
                ),
                children: [
                  TileLayer(
                    urlTemplate: 'https://tile.openstreetmap.org/{z}/{x}/{y}.png',
                    userAgentPackageName: 'com.himaxym.flightRadarCompanion',
                  ),
                  MarkerLayer(markers: [
                    Marker(
                      point: LatLng(_a.lat, _a.lon),
                      width: 36, height: 36,
                      child: Transform.rotate(
                        // Icons.flight points up (=0°); rotate by true track.
                        angle: (_a.track ?? 0) * math.pi / 180,
                        child: const Icon(Icons.flight,
                            size: 32, color: Colors.indigo),
                      ),
                    ),
                    if (_observer != null)
                      Marker(
                        point: _observer!,
                        width: 14, height: 14,
                        child: Container(
                          decoration: BoxDecoration(
                            color: Colors.blue,
                            shape: BoxShape.circle,
                            border: Border.all(color: Colors.white, width: 2),
                          ),
                        ),
                      ),
                  ]),
                ],
              ),
            ),
          ),
          const Padding(
            padding: EdgeInsets.only(top: 2),
            child: Text('© OpenStreetMap contributors',
                style: TextStyle(fontSize: 9, color: Colors.black45)),
          ),
        ],
      ],
    );
  }

  Widget _field(String label, String value) => Padding(
        padding: const EdgeInsets.symmetric(vertical: 4),
        child: Row(
          mainAxisAlignment: MainAxisAlignment.spaceBetween,
          children: [
            Text(label, style: const TextStyle(color: Colors.black54)),
            Text(value, style: const TextStyle(fontWeight: FontWeight.w600)),
          ],
        ),
      );

  Widget _badge(String text, Color color) => Container(
        padding: const EdgeInsets.symmetric(horizontal: 6, vertical: 1),
        decoration: BoxDecoration(
            color: color, borderRadius: BorderRadius.circular(4)),
        child: Text(text,
            style: const TextStyle(
                color: Colors.white, fontSize: 11, fontWeight: FontWeight.bold)),
      );
}

class _Photo extends StatelessWidget {
  final Future<PhotoRef?> photo;
  const _Photo({required this.photo});

  @override
  Widget build(BuildContext context) {
    return FutureBuilder<PhotoRef?>(
      future: photo,
      builder: (context, snap) {
        final p = snap.data;
        if (p == null) {
          return Container(
            height: 160,
            decoration: BoxDecoration(
                color: Colors.black12, borderRadius: BorderRadius.circular(8)),
            alignment: Alignment.center,
            child: const Icon(Icons.flight, size: 48, color: Colors.black38),
          );
        }
        return Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            ClipRRect(
              borderRadius: BorderRadius.circular(8),
              child: Image.network(p.thumbUrl,
                  height: 180, fit: BoxFit.cover,
                  errorBuilder: (_, _, _) => Container(
                      height: 160,
                      color: Colors.black12,
                      child: const Icon(Icons.flight,
                          size: 48, color: Colors.black38))),
            ),
            Text('© ${p.photographer} / planespotters.net',
                style: const TextStyle(fontSize: 9, color: Colors.black45)),
          ],
        );
      },
    );
  }
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd companion && flutter test test/aircraft_detail_sheet_test.dart`
Expected: 4 tests PASS

- [ ] **Step 5: Commit**

```bash
git add companion/lib/ui/aircraft_detail_sheet.dart companion/test/aircraft_detail_sheet_test.dart
git commit -m "feat(companion): live aircraft detail sheet with photo, field grid, mini-map"
```

---

### Task 9: Card tap → sheet

**Files:**
- Modify: `companion/lib/ui/aircraft_card.dart` (add `onTap`)
- Modify: `companion/lib/ui/home_screen.dart` (wire it)
- Modify: `companion/test/aircraft_card_test.dart` (tap test)

- [ ] **Step 1: Write the failing test**

Add to `companion/test/aircraft_card_test.dart` inside `main()` (reuse the file's existing imports/fixtures style):

```dart
  testWidgets('card invokes onTap', (tester) async {
    final photos = PhotoClient(MockClient((_) async => http.Response('{"photos":[]}', 200)));
    const a = Aircraft(
      callsign: 'TST123', type: 'B738', lat: 51.5, lon: -0.45,
      altFt: 30000, gsKt: 400, onGround: false,
    );
    var tapped = false;
    await tester.pumpWidget(MaterialApp(
        home: Scaffold(
            body: AircraftCard(
                aircraft: a, photos: photos, onTap: () => tapped = true))));
    await tester.pump();
    await tester.tap(find.byType(AircraftCard));
    expect(tapped, isTrue);
  });
```

Run: `cd companion && flutter test test/aircraft_card_test.dart`
Expected: FAIL — `onTap` isn't a parameter

- [ ] **Step 2: Add `onTap` to the card**

In `companion/lib/ui/aircraft_card.dart`:

```dart
class AircraftCard extends StatelessWidget {
  final Aircraft aircraft;
  final PhotoClient photos;
  final VoidCallback? onTap;
  const AircraftCard(
      {super.key, required this.aircraft, required this.photos, this.onTap});
```

and wrap the card body (the `Padding` child of `Card`) in an `InkWell`:

```dart
    return Card(
      margin: const EdgeInsets.symmetric(horizontal: 12, vertical: 6),
      child: InkWell(
        onTap: onTap,
        child: Padding(
          // ... existing Padding subtree unchanged, re-indented one level
        ),
      ),
    );
```

- [ ] **Step 3: Wire it in `home_screen.dart`**

Add the import:

```dart
import 'aircraft_detail_sheet.dart';
```

Replace the `SliverList.builder` itemBuilder:

```dart
            SliverList.builder(
              itemCount: _status.aircraft.length,
              itemBuilder: (context, i) {
                final a = _status.aircraft[i];
                return AircraftCard(
                  key: ValueKey(a.hex),
                  aircraft: a,
                  photos: _photos,
                  onTap: () =>
                      showAircraftDetail(context, a, _photos, _controller.status),
                );
              },
            ),
```

- [ ] **Step 4: Run the full Flutter suite**

Run: `cd companion && flutter test`
Expected: all PASS

- [ ] **Step 5: Commit**

```bash
git add companion/lib/ui/aircraft_card.dart companion/lib/ui/home_screen.dart companion/test/aircraft_card_test.dart
git commit -m "feat(companion): tap aircraft card to open the detail sheet"
```

---

### Task 10: Docs + final verification

**Files:**
- Modify: `README.md`, `docs/ARCHITECTURE.md`, `CLAUDE.md`, `companion/README.md`

- [ ] **Step 1: Update docs**

In each file, find the BLE-characteristic / companion-feature passages and extend them factually (follow each file's existing tone; they all already describe `f1a90003`):
- new characteristic `f1a90004` (WRITE+NOTIFY): scan request `"WS"+ver`; per-network notify `"WN"+ver+total+index+rssi+secured+ssidLen+ssid` (≤40 B); `total=0` = none found; dedup/RSSI-sort/cap-15 on device; async scan from `loop()`.
- companion: Scan button → network picker (networks the *device* sees — iOS can't scan Wi-Fi); tap an aircraft card → live detail sheet (photo, full data grid, OSM mini-map).
- In `CLAUDE.md`, update the test counts in the Toolchain section with the real numbers from Step 2 below, and add `wifi_scan_core.h` to the Code layout list.

- [ ] **Step 2: Full verification**

```bash
pio test -e native -f test_core          # expect 56 cases PASS
pio run -e esp32-s3                      # expect SUCCESS
cd companion && flutter analyze          # expect no issues
flutter test                             # expect all PASS (note the count)
flutter build ios --release              # expect ✓ Built Runner.app
flutter build apk --release              # expect ✓ Built app-release.apk
```

- [ ] **Step 3: Commit**

```bash
git add README.md docs/ARCHITECTURE.md CLAUDE.md companion/README.md
git commit -m "docs: wifi-scan characteristic f1a90004 + companion picker & detail sheet"
```

- [ ] **Step 4: Hardware smoke test (manual, Denys)**

Flash + run on devices — out of scope for the executing agent; checklist for the human:
1. `pio run -e esp32-s3 -t upload`; companion on iPhone (`flutter run --release`).
2. Stop feeding → Scan → picker shows real networks sorted by signal → tap → SSID filled → send → device joins.
3. Start feeding → tap an aircraft → sheet shows live data + map; wait for the aircraft to leave range → "Signal lost".
4. Same pass on Android (Xiaomi: `adb install -r -t`).
