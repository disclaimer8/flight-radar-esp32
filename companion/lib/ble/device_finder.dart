import 'dart:async';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';

final Guid radarServiceUuid = Guid('f1a90001-7e1d-4c2a-9b3f-1a2b3c4d5e6f');

/// BLE-scan for the FlightRadar device by service UUID. Returns null when the
/// adapter is off or nothing is found within the timeout.
Future<BluetoothDevice?> findRadarDevice() async {
  final on = await FlutterBluePlus.adapterState
      .where((s) => s == BluetoothAdapterState.on)
      .first
      .timeout(const Duration(seconds: 5),
          onTimeout: () => BluetoothAdapterState.off);
  if (on != BluetoothAdapterState.on) return null;
  final completer = Completer<BluetoothDevice?>();
  final sub = FlutterBluePlus.onScanResults.listen((results) {
    if (results.isNotEmpty && !completer.isCompleted) {
      completer.complete(results.first.device);
    }
  });
  await FlutterBluePlus.startScan(
      withServices: [radarServiceUuid], timeout: const Duration(seconds: 15));
  final device = await completer.future
      .timeout(const Duration(seconds: 16), onTimeout: () => null);
  await sub.cancel();
  await FlutterBluePlus.stopScan();
  return device;
}
