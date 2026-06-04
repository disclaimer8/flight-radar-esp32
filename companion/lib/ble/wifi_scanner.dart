import 'dart:async';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import '../packet/wifi_scan_packet.dart';
import 'device_finder.dart';

class WifiScanException implements Exception {
  final String message;
  const WifiScanException(this.message);
  @override
  String toString() => message;
}

/// On-demand "which networks can the device see" scan, mirroring
/// WifiProvisioner's connect flow: find device, subscribe to the scan
/// characteristic, write a request, collect record notifies. The device is a
/// single-central peripheral, so the feeder must be stopped before calling.
class WifiScanner {
  static final Guid wifiScanUuid = Guid('f1a90004-7e1d-4c2a-9b3f-1a2b3c4d5e6f');

  /// Networks visible to the device, strongest first. Throws
  /// [WifiScanException] with a user-facing reason on any failure.
  Future<List<WifiNetwork>> scan() async {
    BluetoothDevice? device;
    StreamSubscription<List<int>>? notifySub;
    try {
      device = await findRadarDevice();
      if (device == null) throw const WifiScanException('device not found');
      await device.connect(
          license: License.nonprofit, timeout: const Duration(seconds: 15));
      final services = await device.discoverServices();
      BluetoothCharacteristic? ch;
      for (final s in services) {
        if (s.uuid == radarServiceUuid) {
          for (final c in s.characteristics) {
            if (c.uuid == wifiScanUuid) ch = c;
          }
        }
      }
      if (ch == null) {
        throw const WifiScanException('characteristic missing (old firmware?)');
      }

      final collector = ScanCollector();
      final done = Completer<void>();
      await ch.setNotifyValue(true);
      notifySub = ch.onValueReceived.listen((bytes) {
        if (collector.add(bytes) && !done.isCompleted) done.complete();
      });
      await ch.write(encodeScanRequest(), withoutResponse: false);
      await done.future.timeout(const Duration(seconds: 15), onTimeout: () {
        throw const WifiScanException('scan timeout');
      });
      return collector.networks;
    } on WifiScanException {
      rethrow;
    } catch (e) {
      throw WifiScanException(e.toString());
    } finally {
      await notifySub?.cancel();
      try {
        await device?.disconnect();
      } catch (_) {}
    }
  }
}
