import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:flight_radar_companion/main.dart';

void main() {
  testWidgets('app renders the home screen title and start button', (tester) async {
    await tester.pumpWidget(const CompanionApp());
    expect(find.text('Flight Radar Companion'), findsOneWidget);
    expect(find.text('Start feeding device'), findsOneWidget);
  });

  testWidgets('home screen does not overflow in landscape', (tester) async {
    // iPhone 13/14-class landscape viewport; a RenderFlex overflow during
    // layout surfaces as a test failure.
    tester.view.physicalSize = const Size(844 * 3, 390 * 3);
    tester.view.devicePixelRatio = 3.0;
    addTearDown(tester.view.reset);

    await tester.pumpWidget(const CompanionApp());
    expect(find.text('Start feeding device'), findsOneWidget);
  });
}
