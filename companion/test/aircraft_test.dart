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
}
