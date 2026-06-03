import 'dart:async';

import 'package:flutter_foreground_task/flutter_foreground_task.dart';
import '../ble/ble_manager.dart';
import '../data/airplanes_client.dart';
import '../location/location_service.dart';
import '../packet/ble_packet.dart';

const int kRadiusNm = 50;

@pragma('vm:entry-point')
void startCallback() {
  FlutterForegroundTask.setTaskHandler(GatewayTaskHandler());
}

/// Runs in the foreground-service isolate. One cycle per repeat event:
/// GPS fix → fetch airplanes.live → encode packet → BLE write.
class GatewayTaskHandler extends TaskHandler {
  final _ble = BleManager();
  final _location = GeolocatorLocationService();
  final _client = AirplanesClient();
  String _lastBle = 'idle';
  int _lastCount = 0;
  String _lastFix = 'no fix';
  StreamSubscription<BleStatus>? _statusSub;
  bool _busy = false;

  @override
  Future<void> onStart(DateTime timestamp, TaskStarter starter) async {
    _statusSub = _ble.status.listen((s) {
      _lastBle = s.name;
      _push();
    });
    await _ble.start();
  }

  @override
  void onRepeatEvent(DateTime timestamp) {
    if (_busy) return;                 // skip the tick if the previous cycle is still running
    _busy = true;
    _cycle().whenComplete(() => _busy = false);
  }

  Future<void> _cycle() async {
    final fix = await _location.currentFix();
    if (fix == null) {
      _lastFix = 'no fix';
      _push();
      return;
    }
    _lastFix = '${fix.lat.toStringAsFixed(4)}, ${fix.lon.toStringAsFixed(4)}';

    try {
      final aircraft = await _client.fetchNearby(fix.lat, fix.lon, kRadiusNm);
      final packet = encodePacket(fix.lat, fix.lon, aircraft);
      final ok = await _ble.sendPacket(packet);
      if (ok) _lastCount = aircraft.length;
      FlutterForegroundTask.updateService(
        notificationTitle: 'Feeding Flight Radar',
        notificationText:
            ok ? 'Sent ${aircraft.length} aircraft' : 'Waiting for device…',
      );
    } catch (_) {
      FlutterForegroundTask.updateService(
        notificationTitle: 'Feeding Flight Radar',
        notificationText: 'No data (offline?)',
      );
    }
    _push();
  }

  void _push() {
    FlutterForegroundTask.sendDataToMain({
      'ble': _lastBle,
      'count': _lastCount,
      'fix': _lastFix,
    });
  }

  @override
  Future<void> onDestroy(DateTime timestamp, bool isTimeout) async {
    await _statusSub?.cancel();
    await _ble.stop();
    await _ble.dispose();
  }

  @override
  void onReceiveData(Object data) {}

  @override
  void onNotificationButtonPressed(String id) {
    if (id == 'stop') FlutterForegroundTask.stopService();
  }

  @override
  void onNotificationPressed() => FlutterForegroundTask.launchApp('/');

  @override
  void onNotificationDismissed() {}
}
