import 'package:flutter_test/flutter_test.dart';
import 'package:flight_radar_companion/main.dart';

void main() {
  testWidgets('app renders the home screen title and start button', (tester) async {
    await tester.pumpWidget(const CompanionApp());
    expect(find.text('Flight Radar Companion'), findsOneWidget);
    expect(find.text('Start feeding device'), findsOneWidget);
  });
}
