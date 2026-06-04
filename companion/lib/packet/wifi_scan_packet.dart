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
