import 'dart:async';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import '../packet/wifi_config_packet.dart';

enum ProvPhase { idle, connecting, sending, applying, connected, failed }

class ProvState {
  final ProvPhase phase;
  final String detail;
  const ProvState(this.phase, [this.detail = '']);
}

/// On-demand BLE Wi-Fi provisioning, independent of the feeder. Scans for the
/// device, connects, writes the credentials to the wifi-config characteristic,
/// maps the status notifications to [ProvState]s, then disconnects. The device is
/// a single-central peripheral, so the feeder must be stopped before calling this.
class WifiProvisioner {
  static final Guid serviceUuid = Guid('f1a90001-7e1d-4c2a-9b3f-1a2b3c4d5e6f');
  static final Guid wifiCfgUuid = Guid('f1a90003-7e1d-4c2a-9b3f-1a2b3c4d5e6f');

  final _states = StreamController<ProvState>.broadcast();
  Stream<ProvState> get states => _states.stream;

  Future<void> provision(String ssid, String pass) async {
    BluetoothDevice? device;
    StreamSubscription<List<int>>? notifySub;
    final done = Completer<void>();
    try {
      _emit(const ProvState(ProvPhase.connecting));
      device = await _scanForDevice();
      if (device == null) {
        _emit(const ProvState(ProvPhase.failed, 'device not found'));
        return;
      }
      await device.connect(license: License.nonprofit, timeout: const Duration(seconds: 15));
      final services = await device.discoverServices();
      BluetoothCharacteristic? ch;
      for (final s in services) {
        if (s.uuid == serviceUuid) {
          for (final c in s.characteristics) {
            if (c.uuid == wifiCfgUuid) ch = c;
          }
        }
      }
      if (ch == null) {
        _emit(const ProvState(ProvPhase.failed, 'characteristic missing'));
        return;
      }

      await ch.setNotifyValue(true);
      notifySub = ch.onValueReceived.listen((bytes) {
        final st = parseWifiStatus(bytes);
        if (st.code == 0) {
          _emit(const ProvState(ProvPhase.applying));
        } else if (st.code == 1) {
          _emit(ProvState(ProvPhase.connected, st.detail));
          if (!done.isCompleted) done.complete();
        } else {
          _emit(ProvState(ProvPhase.failed, st.detail));
          if (!done.isCompleted) done.complete();
        }
      });

      _emit(const ProvState(ProvPhase.sending));
      await ch.write(encodeWifiConfig(ssid, pass), withoutResponse: false);

      await done.future.timeout(const Duration(seconds: 30), onTimeout: () {
        _emit(const ProvState(ProvPhase.failed, 'timeout'));
      });
    } catch (e) {
      _emit(ProvState(ProvPhase.failed, e.toString()));
    } finally {
      await notifySub?.cancel();
      try { await device?.disconnect(); } catch (_) {}
    }
  }

  Future<BluetoothDevice?> _scanForDevice() async {
    await FlutterBluePlus.adapterState.where((s) => s == BluetoothAdapterState.on).first;
    final completer = Completer<BluetoothDevice?>();
    final sub = FlutterBluePlus.onScanResults.listen((results) {
      if (results.isNotEmpty && !completer.isCompleted) completer.complete(results.first.device);
    });
    await FlutterBluePlus.startScan(withServices: [serviceUuid], timeout: const Duration(seconds: 15));
    final device = await completer.future
        .timeout(const Duration(seconds: 16), onTimeout: () => null);
    await sub.cancel();
    await FlutterBluePlus.stopScan();
    return device;
  }

  void _emit(ProvState s) {
    if (!_states.isClosed) _states.add(s);
  }

  void dispose() {
    _states.close();
  }
}
