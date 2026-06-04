import 'dart:async';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:http/testing.dart';
import 'package:http/http.dart' as http;
import 'package:flight_radar_companion/data/aircraft.dart';
import 'package:flight_radar_companion/data/photo_client.dart';
import 'package:flight_radar_companion/service/gateway_engine.dart'
    show GatewayStatus;
import 'package:flight_radar_companion/ui/aircraft_detail_sheet.dart';

PhotoClient noPhotos() =>
    PhotoClient(MockClient((_) async => http.Response('{"photos":[]}', 200)));

const full = Aircraft(
  callsign: 'BAW117', type: 'A388', lat: 51.5, lon: -0.45,
  altFt: 35000, gsKt: 450, onGround: false, track: 270, squawk: 7700,
  registration: 'G-XLEA', origin: 'EGLL', dest: 'KJFK',
  hex: '40612a', desc: 'Airbus A380-841', isEmergency: true, distKm: 8.3,
);

const sparse = Aircraft(
  callsign: '', type: 'GLID', lat: 50.0, lon: 14.0,
  altFt: null, gsKt: null, onGround: true,
);

Widget host(Aircraft a, Stream<GatewayStatus> status) => MaterialApp(
      home: Scaffold(
        body: AircraftDetailSheet(
            aircraft: a, photos: noPhotos(), status: status, showMap: false),
      ),
    );

void main() {
  testWidgets('renders every populated field', (tester) async {
    final status = StreamController<GatewayStatus>.broadcast();
    await tester.pumpWidget(host(full, status.stream));
    await tester.pump();

    expect(find.text('BAW117'), findsOneWidget);
    expect(find.text('Airbus A380-841'), findsOneWidget);
    expect(find.text('EMG'), findsOneWidget);
    expect(find.text('35000 ft'), findsOneWidget);
    expect(find.text('450 kt'), findsOneWidget);
    expect(find.text('270°'), findsOneWidget);
    expect(find.text('7700'), findsOneWidget);
    expect(find.text('EGLL → KJFK'), findsOneWidget);
    expect(find.text('8.3 km'), findsOneWidget);
    expect(find.text('G-XLEA'), findsOneWidget);
    expect(find.text('40612a'), findsOneWidget);
    expect(find.text('51.5000, -0.4500'), findsOneWidget);
    await status.close();
  });

  testWidgets('missing values render as a dash', (tester) async {
    final status = StreamController<GatewayStatus>.broadcast();
    await tester.pumpWidget(host(sparse, status.stream));
    await tester.pump();

    expect(find.text('——'), findsNothing);   // sanity: dashes are single
    expect(find.text('—'), findsWidgets);    // alt, speed, track, squawk, route…
    expect(find.text('Yes'), findsOneWidget); // on ground
    await status.close();
  });

  testWidgets('live update replaces data; disappearance shows signal lost',
      (tester) async {
    final status = StreamController<GatewayStatus>.broadcast();
    await tester.pumpWidget(host(full, status.stream));
    await tester.pump();

    // Same hex, new distance → field updates.
    status.add(GatewayStatus(
        aircraft: [full.copyWith(distKm: 9.9)], fix: '51.0000, -0.5000'));
    await tester.pump();
    expect(find.text('9.9 km'), findsOneWidget);
    expect(find.textContaining('Signal lost'), findsNothing);

    // Aircraft gone from the feed → banner, last data retained.
    status.add(const GatewayStatus(aircraft: []));
    await tester.pump();
    expect(find.textContaining('Signal lost'), findsOneWidget);
    expect(find.text('BAW117'), findsOneWidget);
    await status.close();
  });

  test('parseFix extracts observer coordinates', () {
    expect(parseFix('51.5074, -0.1278')!.latitude, closeTo(51.5074, 1e-6));
    expect(parseFix('51.5074, -0.1278')!.longitude, closeTo(-0.1278, 1e-6));
    expect(parseFix('no fix'), isNull);
    expect(parseFix(''), isNull);
  });
}
