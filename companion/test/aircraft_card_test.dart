import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:http/testing.dart';
import 'package:http/http.dart' as http;
import 'package:flight_radar_companion/data/aircraft.dart';
import 'package:flight_radar_companion/data/photo_client.dart';
import 'package:flight_radar_companion/theme/app_theme.dart';
import 'package:flight_radar_companion/ui/aircraft_card.dart';

Widget _wrap(Widget child) =>
    MaterialApp(theme: AppTheme.light(), home: Scaffold(body: child));

void main() {
  testWidgets('card shows callsign, route, distance, and an emergency badge',
      (tester) async {
    final photos = PhotoClient(MockClient((_) async => http.Response('{"photos":[]}', 200)));
    const a = Aircraft(
      callsign: 'BAW117', type: 'A388', lat: 51.5, lon: -0.45,
      altFt: 35000, gsKt: 450, onGround: false, squawk: 7700,
      registration: 'G-XLEA', origin: 'EGLL', dest: 'KJFK',
      hex: '40612a', desc: 'Airbus A380-841', isEmergency: true, distKm: 8.0,
    );
    await tester.pumpWidget(_wrap(AircraftCard(aircraft: a, photos: photos)));
    await tester.pump();

    expect(find.text('BAW117'), findsOneWidget);
    expect(find.text('EGLL → KJFK'), findsOneWidget);
    expect(find.textContaining('G-XLEA'), findsOneWidget);
    expect(find.text('EMG'), findsOneWidget);
    expect(find.byIcon(Icons.flight), findsOneWidget);
  });

  testWidgets('military card shows MIL badge and no route when route absent',
      (tester) async {
    final photos = PhotoClient(MockClient((_) async => http.Response('{"photos":[]}', 200)));
    const a = Aircraft(
      callsign: 'RRR2745', type: 'A400', lat: 51, lon: -1, altFt: 8000, gsKt: 300,
      onGround: false, hex: '43c123', desc: 'Airbus A400M', isMilitary: true, distKm: 20.0,
    );
    await tester.pumpWidget(_wrap(AircraftCard(aircraft: a, photos: photos)));
    await tester.pump();

    expect(find.text('RRR2745'), findsOneWidget);
    expect(find.text('MIL'), findsOneWidget);
    expect(find.textContaining('→'), findsNothing);
  });

  testWidgets('card invokes onTap', (tester) async {
    final photos = PhotoClient(MockClient((_) async => http.Response('{"photos":[]}', 200)));
    const a = Aircraft(
      callsign: 'TST123', type: 'B738', lat: 51.5, lon: -0.45,
      altFt: 30000, gsKt: 400, onGround: false,
    );
    var tapped = false;
    await tester.pumpWidget(_wrap(
        AircraftCard(aircraft: a, photos: photos, onTap: () => tapped = true)));
    await tester.pump();
    await tester.tap(find.byType(AircraftCard));
    expect(tapped, isTrue);
  });
}
