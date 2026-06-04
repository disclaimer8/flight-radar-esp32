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
