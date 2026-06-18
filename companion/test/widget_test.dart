import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:shared_preferences/shared_preferences.dart';
import 'package:flight_radar_companion/main.dart';

void main() {
  setUp(() {
    // Skip the first-run onboarding so HomeScreen's post-frame SharedPreferences
    // read (and the permission priming) doesn't fire during widget tests.
    SharedPreferences.setMockInitialValues({'onboarding_seen_v1': true});
  });

  testWidgets('app renders the home screen title and start button', (tester) async {
    await tester.pumpWidget(const CompanionApp());
    expect(find.text('Flight Radar'), findsOneWidget);
    expect(find.text('Start'), findsOneWidget);
  });

  testWidgets('wifi setup section exposes a scan-networks button when expanded',
      (tester) async {
    await tester.pumpWidget(const CompanionApp());
    await tester.tap(find.text('Device Wi-Fi setup'));
    await tester.pumpAndSettle();
    expect(find.byIcon(Icons.wifi_find), findsOneWidget);
  });

  testWidgets('home screen does not overflow in landscape', (tester) async {
    // iPhone 13/14-class landscape viewport; a RenderFlex overflow during
    // layout surfaces as a test failure.
    tester.view.physicalSize = const Size(844 * 3, 390 * 3);
    tester.view.devicePixelRatio = 3.0;
    addTearDown(tester.view.reset);

    await tester.pumpWidget(const CompanionApp());
    expect(find.text('Start'), findsOneWidget);
  });
}
