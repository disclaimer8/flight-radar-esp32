import 'package:flutter_test/flutter_test.dart';
import 'package:http/http.dart' as http;
import 'package:http/testing.dart';
import 'package:flight_radar_companion/data/photo_client.dart';

String _photoJson(String src) => '{"photos":[{"thumbnail_large":{"src":"$src","size":{"width":419,"height":280}},'
    '"link":"https://planespotters.net/photo/1","photographer":"Jane Doe"}]}';
const _empty = '{"photos":[]}';

void main() {
  test('lookup hits by registration', () async {
    var calls = 0;
    final c = PhotoClient(MockClient((req) async {
      calls++;
      expect(req.headers['User-Agent'], contains('flight-radar-esp32-companion'));
      expect(req.url.path, '/pub/photos/reg/D-AIMA');
      return http.Response(_photoJson('https://t/x.jpg'), 200);
    }));
    final p = await c.lookup(reg: 'D-AIMA', hex: '3c4ad2');
    expect(p, isNotNull);
    expect(p!.thumbUrl, 'https://t/x.jpg');
    expect(p.photographer, 'Jane Doe');
    expect(calls, 1);
  });

  test('lookup falls back to hex when reg has no photo', () async {
    final c = PhotoClient(MockClient((req) async {
      if (req.url.path.contains('/reg/')) return http.Response(_empty, 200);
      return http.Response(_photoJson('https://t/h.jpg'), 200);
    }));
    final p = await c.lookup(reg: 'NOREG', hex: '3c4ad2');
    expect(p, isNotNull);
    expect(p!.thumbUrl, 'https://t/h.jpg');
  });

  test('both miss -> null, cached (no second HTTP call)', () async {
    var calls = 0;
    final c = PhotoClient(MockClient((req) async {
      calls++;
      return http.Response(_empty, 200);
    }));
    expect(await c.lookup(reg: 'NOPE', hex: 'beef'), isNull);
    final before = calls;
    expect(await c.lookup(reg: 'NOPE', hex: 'beef'), isNull);
    expect(calls, before);
  });

  test('non-200 -> null', () async {
    final c = PhotoClient(MockClient((req) async => http.Response('nope', 403)));
    expect(await c.lookup(reg: 'X', hex: ''), isNull);
  });
}
