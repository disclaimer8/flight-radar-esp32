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
