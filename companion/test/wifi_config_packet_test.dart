import 'package:flutter_test/flutter_test.dart';
import 'package:flight_radar_companion/packet/wifi_config_packet.dart';

void main() {
  test('encodeWifiConfig builds the byte-exact WC packet', () {
    final bytes = encodeWifiConfig('MyNet', 'secret');
    expect(bytes, [0x57, 0x43, 0x01, 5, 77, 121, 78, 101, 116, 6, 115, 101, 99, 114, 101, 116]);
  });

  test('encodeWifiConfig supports an empty password (open network)', () {
    final bytes = encodeWifiConfig('AP', '');
    expect(bytes, [0x57, 0x43, 0x01, 2, 65, 80, 0]);
  });

  test('parseWifiStatus decodes code + detail', () {
    expect(parseWifiStatus([0]).code, 0);
    final ok = parseWifiStatus([1, 49, 57, 50, 46, 49]);
    expect(ok.code, 1);
    expect(ok.detail, '192.1');
    final fail = parseWifiStatus([2]);
    expect(fail.code, 2);
    expect(fail.detail, '');
    expect(parseWifiStatus([]).code, 2);
  });
}
