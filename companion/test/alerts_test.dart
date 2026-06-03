import 'package:flutter_test/flutter_test.dart';
import 'package:flight_radar_companion/data/aircraft.dart';
import 'package:flight_radar_companion/service/alerts.dart';

Aircraft _ac({String cs = 'AAA', String type = 'A320', String hex = 'abc123',
        bool mil = false, bool emg = false, int? squawk}) =>
    Aircraft(callsign: cs, type: type, lat: 0, lon: 0, altFt: 1000, gsKt: 200,
        onGround: false, squawk: squawk, hex: hex, isMilitary: mil, isEmergency: emg);

void main() {
  test('isAlertWorthy is true for emergency or military only', () {
    expect(isAlertWorthy(_ac(emg: true)), isTrue);
    expect(isAlertWorthy(_ac(mil: true)), isTrue);
    expect(isAlertWorthy(_ac()), isFalse);
  });

  test('computeNewAlerts flags first sighting then de-dups', () {
    final mil = _ac(cs: 'RRR1', hex: 'h1', mil: true);
    final r1 = computeNewAlerts([mil], <String>{});
    expect(r1.newAlerts.map((a) => a.hex), ['h1']);
    expect(r1.alerted, {'h1'});
    final r2 = computeNewAlerts([mil], r1.alerted);
    expect(r2.newAlerts, isEmpty);
    expect(r2.alerted, {'h1'});
  });

  test('computeNewAlerts re-alerts after the aircraft leaves and returns', () {
    final mil = _ac(hex: 'h1', mil: true);
    final gone = computeNewAlerts(const [], {'h1'});
    expect(gone.alerted, isEmpty);
    final back = computeNewAlerts([mil], gone.alerted);
    expect(back.newAlerts.map((a) => a.hex), ['h1']);
  });

  test('computeNewAlerts ignores non-worthy and empty-hex aircraft', () {
    final plain = _ac(hex: 'p1');
    final noHex = _ac(hex: '', mil: true);
    final r = computeNewAlerts([plain, noHex], <String>{});
    expect(r.newAlerts, isEmpty);
    expect(r.alerted, isEmpty);
  });

  test('alert text: emergency takes precedence, includes squawk', () {
    final e = _ac(cs: 'BAW117', emg: true, squawk: 7700);
    expect(alertTitle(e), 'Emergency squawk');
    expect(alertBody(e), contains('7700'));
    expect(alertBody(e), contains('BAW117'));
    final m = _ac(cs: 'RRR2745', type: 'A400', mil: true);
    expect(alertTitle(m), 'Military aircraft');
    expect(alertBody(m), contains('RRR2745'));
    expect(alertBody(m), contains('A400'));
  });

  test('alertBody exact format incl. no-squawk and empty callsign', () {
    expect(alertBody(_ac(cs: 'BAW117', emg: true, squawk: 7700)), '🚨 7700: BAW117');
    expect(alertBody(_ac(cs: 'BAW117', emg: true)), '🚨 BAW117'); // no squawk
    expect(alertBody(_ac(cs: '', emg: true, squawk: 7600)), '🚨 7600: ------'); // empty callsign
    expect(alertBody(_ac(cs: '', type: 'A400', mil: true)), '------ A400'); // empty callsign, military
  });
}
