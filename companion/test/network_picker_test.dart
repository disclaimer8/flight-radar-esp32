import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:flight_radar_companion/packet/wifi_scan_packet.dart';
import 'package:flight_radar_companion/ui/network_picker.dart';

void main() {
  testWidgets('lists networks with lock icons and returns the tapped one',
      (tester) async {
    WifiNetwork? picked;
    await tester.pumpWidget(MaterialApp(
      home: Builder(
        builder: (context) => ElevatedButton(
          onPressed: () async {
            picked = await showModalBottomSheet<WifiNetwork>(
              context: context,
              builder: (_) => const NetworkPicker(networks: [
                WifiNetwork('HomeNet', -55, true),
                WifiNetwork('CoffeeShop', -82, false),
              ]),
            );
          },
          child: const Text('open'),
        ),
      ),
    ));
    await tester.tap(find.text('open'));
    await tester.pumpAndSettle();

    expect(find.text('HomeNet'), findsOneWidget);
    expect(find.text('CoffeeShop'), findsOneWidget);
    expect(find.byIcon(Icons.lock_outline), findsOneWidget); // only HomeNet is secured

    await tester.tap(find.text('HomeNet'));
    await tester.pumpAndSettle();
    expect(picked, isNotNull);
    expect(picked!.ssid, 'HomeNet');
  });
}
