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

  test('parseAircraft caps to 10 nearest', () {
    final acs = List.generate(20, (i) =>
        '{"flight":"F$i","t":"A320","lat":${48.0 + i * 0.1},"lon":11.0,"alt_baro":1000,"gs":300}');
    final body = '{"ac":[${acs.join(",")}]}';
    final list = parseAircraft(body, 48.0, 11.0);
    expect(list.length, 10);
    expect(list.first.callsign, 'F0'); // nearest
  });

  test('parseAircraft tolerates empty / missing ac array', () {
    expect(parseAircraft('{"ac":[]}', 0, 0), isEmpty);
    expect(parseAircraft('{}', 0, 0), isEmpty);
  });

  test('parseAircraft skips entries missing lat or lon', () {
    const body = '''
{"ac":[
  {"flight":"HASLL","t":"A320","lat":48.1,"lon":11.0,"alt_baro":1000,"gs":300},
  {"flight":"NOLAT","t":"A320","lon":11.0,"alt_baro":1000,"gs":300},
  {"flight":"NOLON","t":"A320","lat":48.2,"alt_baro":1000,"gs":300}
]}
''';
    final list = parseAircraft(body, 48.0, 11.0);
    expect(list.length, 1);
    expect(list.single.callsign, 'HASLL');
  });

  test('parseAircraft skips non-object entries in the ac array', () {
    const body = '{"ac":[null, 5, "x", {"flight":"OK1","t":"A320","lat":48.1,"lon":11.0,"alt_baro":1000,"gs":300}]}';
    final list = parseAircraft(body, 48.0, 11.0);
    expect(list.length, 1);
    expect(list.single.callsign, 'OK1');
  });

  test('parseAircraft hides on-ground aircraft when hideGround is true', () {
    const body = '''
{"ac":[
  {"flight":"GND1","t":"B772","lat":0.0,"lon":0.1,"alt_baro":"ground","gs":3},
  {"flight":"AIR1","t":"A320","lat":0.0,"lon":0.2,"alt_baro":10000,"gs":300},
  {"flight":"AIR2","t":"A320","lat":0.0,"lon":0.3,"alt_baro":20000,"gs":400}
]}
''';
    // hideGround true: GND (nearest) excluded; two nearest airborne kept.
    final hidden = parseAircraft(body, 0.0, 0.0, hideGround: true);
    expect(hidden.map((a) => a.callsign), ['AIR1', 'AIR2']);
    // hideGround false (default): nearest-first incl. the ground aircraft.
    final all = parseAircraft(body, 0.0, 0.0);
    expect(all.first.callsign, 'GND1');
  });

  test('parseAircraft extracts track and squawk', () {
    const body = '{"ac":[{"flight":"AB","t":"A320","lat":0.0,"lon":0.1,'
        '"alt_baro":10000,"gs":300,"track":275.4,"squawk":"7700"}]}';
    final a = parseAircraft(body, 0.0, 0.0).single;
    expect(a.track, closeTo(275.4, 0.1));
    expect(a.squawk, 7700);
    // Missing -> null.
    final b = parseAircraft('{"ac":[{"flight":"X","lat":0.0,"lon":0.1}]}', 0.0, 0.0).single;
    expect(b.track, isNull);
    expect(b.squawk, isNull);
  });

  test('parseAircraft extracts registration', () {
    final a = parseAircraft('{"ac":[{"flight":"BAW1","r":"G-XLEA","lat":0.0,"lon":0.1}]}', 0, 0).single;
    expect(a.registration, 'G-XLEA');
    final b = parseAircraft('{"ac":[{"flight":"X","lat":0.0,"lon":0.1}]}', 0, 0).single;
    expect(b.registration, isNull);
  });

  test('parseHexdbRoute splits ICAO route', () {
    expect(parseHexdbRoute('EGLL-KJFK'), ('EGLL', 'KJFK'));
    expect(parseHexdbRoute('EGLL-LEMD-EGLL'), ('EGLL', 'EGLL'));
    expect(parseHexdbRoute(''), ('', ''));
  });
}
