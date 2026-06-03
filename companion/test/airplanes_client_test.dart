import 'package:flutter_test/flutter_test.dart';
import 'package:flight_radar_companion/data/airplanes_client.dart';

const _json = '''
{"ac":[
  {"flight":"RYR9XZ ","t":"B738","lat":48.50,"lon":11.00,"alt_baro":35000,"gs":453},
  {"flight":"DLH4AB ","t":"A320","lat":48.10,"lon":11.00,"alt_baro":12000,"gs":380},
  {"flight":"GRND01 ","t":"B772","lat":48.02,"lon":11.00,"alt_baro":"ground","gs":3},
  {"flight":"NOFIELD","t":"E190","lat":48.30,"lon":11.00}
]}
''';

void main() {
  test('parseAircraft maps fields, sorts nearest-first, and handles ground/missing', () {
    final list = parseAircraft(_json, 48.0, 11.0);
    expect(list.length, 4);
    expect(list.first.callsign, 'GRND01'); // 48.02 is nearest
    expect(list.first.onGround, isTrue);
    expect(list.first.altFt, isNull);      // "ground" → alt invalid
    final ryr = list.firstWhere((a) => a.callsign == 'RYR9XZ');
    expect(ryr.type, 'B738');
    expect(ryr.altFt, 35000);
    expect(ryr.gsKt, 453);
    final noField = list.firstWhere((a) => a.callsign == 'NOFIELD');
    expect(noField.altFt, isNull);
    expect(noField.gsKt, isNull);
  });

  test('parseAircraft caps to 16 nearest', () {
    final acs = List.generate(20, (i) =>
        '{"flight":"F$i","t":"A320","lat":${48.0 + i * 0.1},"lon":11.0,"alt_baro":1000,"gs":300}');
    final body = '{"ac":[${acs.join(",")}]}';
    final list = parseAircraft(body, 48.0, 11.0);
    expect(list.length, 16);
    expect(list.first.callsign, 'F0'); // nearest
  });

  test('parseAircraft tolerates empty / missing ac array', () {
    expect(parseAircraft('{"ac":[]}', 0, 0), isEmpty);
    expect(parseAircraft('{}', 0, 0), isEmpty);
  });
}
