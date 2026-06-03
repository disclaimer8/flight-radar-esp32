import 'package:flutter_test/flutter_test.dart';
import 'package:flight_radar_companion/data/aircraft.dart';

void main() {
  test('haversineKm matches a known city pair (London–Paris ~343 km)', () {
    final d = haversineKm(51.5074, -0.1278, 48.8566, 2.3522);
    expect(d, closeTo(343, 5));
  });

  test('haversineKm is zero for identical points', () {
    expect(haversineKm(38.0, -9.0, 38.0, -9.0), closeTo(0, 0.001));
  });

  test('Aircraft holds its fields', () {
    const a = Aircraft(
      callsign: 'RYR4KP', type: 'B738',
      lat: 38.8, lon: -9.28, altFt: 12000, gsKt: 420, onGround: false,
    );
    expect(a.callsign, 'RYR4KP');
    expect(a.altFt, 12000);
    expect(a.onGround, isFalse);
  });

  test('Aircraft holds the new enrichment fields', () {
    const a = Aircraft(
      callsign: 'RRR2745', type: 'A400', lat: 51.0, lon: -1.0,
      altFt: 8000, gsKt: 300, onGround: false,
      hex: '43c123', desc: 'Airbus A400M', isMilitary: true,
      isEmergency: false, distKm: 12.5,
    );
    expect(a.hex, '43c123');
    expect(a.desc, 'Airbus A400M');
    expect(a.isMilitary, isTrue);
    expect(a.isEmergency, isFalse);
    expect(a.distKm, 12.5);
  });

  test('Aircraft toJson/fromJson round-trips all fields incl. nulls', () {
    const a = Aircraft(
      callsign: 'BAW117', type: 'A388', lat: 51.5, lon: -0.45,
      altFt: 35000, gsKt: 450, onGround: false, track: 287.0, squawk: 7700,
      registration: 'G-XLEA', origin: 'EGLL', dest: 'KJFK',
      hex: '40612a', desc: 'Airbus A380-841', isMilitary: false,
      isEmergency: true, distKm: 8.0,
    );
    final b = Aircraft.fromJson(a.toJson());
    expect(b.callsign, a.callsign);
    expect(b.lat, a.lat);
    expect(b.altFt, a.altFt);
    expect(b.track, a.track);
    expect(b.squawk, a.squawk);
    expect(b.registration, a.registration);
    expect(b.origin, a.origin);
    expect(b.dest, a.dest);
    expect(b.hex, a.hex);
    expect(b.desc, a.desc);
    expect(b.isEmergency, a.isEmergency);
    expect(b.distKm, a.distKm);
    expect(b.gsKt, a.gsKt);
    expect(b.onGround, a.onGround);
    expect(b.isMilitary, a.isMilitary);

    const c = Aircraft(callsign: 'X', type: '', lat: 0, lon: 0,
        altFt: null, gsKt: null, onGround: true);
    final d = Aircraft.fromJson(c.toJson());
    expect(d.altFt, isNull);
    expect(d.gsKt, isNull);
    expect(d.registration, isNull);
    expect(d.hex, '');
    expect(d.isMilitary, isFalse);
    expect(d.distKm, isNull);
  });
}
