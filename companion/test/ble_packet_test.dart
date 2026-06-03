import 'dart:typed_data';
import 'package:flutter_test/flutter_test.dart';
import 'package:flight_radar_companion/data/aircraft.dart';
import 'package:flight_radar_companion/packet/ble_packet.dart';

Aircraft _ac({
  String cs = 'AAA', String ty = 'A320',
  double lat = 0, double lon = 0, int? alt = 1000, int? gs = 300, bool ground = false,
}) => Aircraft(callsign: cs, type: ty, lat: lat, lon: lon, altFt: alt, gsKt: gs, onGround: ground);

void main() {
  test('empty packet is a 12-byte header', () {
    final bytes = encodePacket(48.0, 11.0, const []);
    expect(bytes.length, 12);
    expect(bytes[0], 0x46); // 'F'
    expect(bytes[1], 0x52); // 'R'
    expect(bytes[2], 1);    // version
    expect(bytes[3], 0);    // count
    final bd = ByteData.sublistView(bytes);
    expect(bd.getFloat32(4, Endian.little), closeTo(48.0, 0.0001));
    expect(bd.getFloat32(8, Endian.little), closeTo(11.0, 0.0001));
  });

  test('one record encodes fields at the correct offsets', () {
    final bytes = encodePacket(48.0, 11.0, [
      _ac(cs: 'RYR9XZ', ty: 'B738', lat: 48.1, lon: 11.2, alt: 12000, gs: 380, ground: false),
    ]);
    expect(bytes.length, 12 + 28);
    expect(bytes[3], 1); // count
    final r = ByteData.sublistView(bytes, 12);
    expect(String.fromCharCodes(bytes.sublist(12, 20)), 'RYR9XZ  ');
    expect(String.fromCharCodes(bytes.sublist(20, 24)), 'B738');
    expect(r.getFloat32(12, Endian.little), closeTo(48.1, 0.0001));
    expect(r.getFloat32(16, Endian.little), closeTo(11.2, 0.0001));
    expect(r.getInt32(20, Endian.little), 12000);
    expect(r.getInt16(24, Endian.little), 380);
    expect(r.getUint8(26), bleFlagAltValid | bleFlagGsValid);
    expect(r.getUint8(27), 0);
  });

  test('ground aircraft sets GROUND flag and clears alt/gs valid bits', () {
    final bytes = encodePacket(0, 0, [_ac(cs: 'GND1', alt: null, gs: null, ground: true)]);
    final flags = bytes[12 + 26];
    expect(flags & bleFlagGround, bleFlagGround);
    expect(flags & bleFlagAltValid, 0);
    expect(flags & bleFlagGsValid, 0);
  });

  test('long callsign/type are truncated to field width', () {
    final bytes = encodePacket(0, 0, [_ac(cs: 'TOOLONGCALL', ty: 'TYPED')]);
    expect(String.fromCharCodes(bytes.sublist(12, 20)), 'TOOLONGC');
    expect(String.fromCharCodes(bytes.sublist(20, 24)), 'TYPE');
  });

  test('count caps at 16 even if given more', () {
    final many = List.generate(20, (i) => _ac(cs: 'A$i'));
    final bytes = encodePacket(0, 0, many);
    expect(bytes[3], 16);
    expect(bytes.length, 12 + 16 * 28);
  });
}
