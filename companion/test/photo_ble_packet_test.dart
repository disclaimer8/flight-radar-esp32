import 'package:flutter_test/flutter_test.dart';
import 'package:flight_radar_companion/packet/photo_ble_packet.dart';

void main() {
  test('parses a PR request', () {
    final bytes = [0x50, 0x52, 1, 9, 7, ...'ABC-123'.codeUnits];
    final r = parsePhotoReq(bytes)!;
    expect(r.reqId, 9);
    expect(r.key, 'ABC-123');
  });

  test('rejects malformed PR', () {
    expect(parsePhotoReq([]), isNull);
    expect(parsePhotoReq([0x50, 0x52, 1, 1]), isNull);              // no keyLen byte/key
    expect(parsePhotoReq([0x50, 0x48, 1, 1, 1, 65]), isNull);       // wrong type
    expect(parsePhotoReq([0x50, 0x52, 2, 1, 1, 65]), isNull);       // wrong version
    expect(parsePhotoReq([0x50, 0x52, 1, 1, 0]), isNull);           // zero keyLen
    expect(parsePhotoReq([0x50, 0x52, 1, 1, 5, 65]), isNull);       // truncated key
  });

  test('buildPhotoHeader encodes len LE + credit', () {
    final h = buildPhotoHeader(3, 5000, 'Jane');
    expect(h.sublist(0, 4), [0x50, 0x48, 1, 3]);
    expect(h[4], 5000 & 0xFF);          // 0x88
    expect(h[5], (5000 >> 8) & 0xFF);   // 0x13
    expect(h[6], 0); expect(h[7], 0);
    expect(h[8], 'Jane'.length);
    expect(String.fromCharCodes(h.sublist(9)), 'Jane');
  });

  test('buildPhotoChunk encodes seq LE + payload', () {
    final c = buildPhotoChunk(4, 2, [0xDE, 0xAD]);
    expect(c.sublist(0, 4), [0x50, 0x44, 1, 4]);
    expect(c[4], 2); expect(c[5], 0);
    expect(c.sublist(6), [0xDE, 0xAD]);
  });

  test('chunkJpeg splits by maxPayload with rising seq', () {
    final jpeg = List<int>.generate(250, (i) => i & 0xFF);
    final frames = chunkJpeg(7, jpeg, 100);
    expect(frames.length, 3);                 // 100 + 100 + 50
    expect(frames[0][4], 0);                  // seq 0
    expect(frames[2][4], 2);                  // seq 2
    expect(frames[2].length, 6 + 50);
  });

  test('buildProxiedPhotoUrl strips scheme + adds quality', () {
    final u = buildProxiedPhotoUrl('https://t.plnspttrs.net/x/y.jpg', quality: 55);
    expect(u, 'https://wsrv.nl/?url=t.plnspttrs.net/x/y.jpg&w=240&h=240&fit=cover&output=jpg&q=55');
  });
}
