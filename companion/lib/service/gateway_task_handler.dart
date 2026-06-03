import 'dart:async';
import 'package:flutter_foreground_task/flutter_foreground_task.dart';
import 'gateway_engine.dart';

@pragma('vm:entry-point')
void startCallback() {
  FlutterForegroundTask.setTaskHandler(GatewayTaskHandler());
}

/// Android driver: hosts the GatewayEngine in the foreground-service isolate,
/// forwards engine status to the UI isolate, and drives a cycle per repeat event.
class GatewayTaskHandler extends TaskHandler {
  final GatewayEngine _engine = GatewayEngine();
  StreamSubscription<GatewayStatus>? _sub;

  @override
  Future<void> onStart(DateTime timestamp, TaskStarter starter) async {
    _sub = _engine.status.listen((s) {
      FlutterForegroundTask.sendDataToMain({'ble': s.ble, 'count': s.count, 'fix': s.fix});
      final connected = s.ble == 'connected';
      FlutterForegroundTask.updateService(
        notificationTitle: 'Feeding Flight Radar',
        notificationText: connected ? 'Sent ${s.count} aircraft' : 'Waiting for device…',
      );
    });
    await _engine.start();
  }

  @override
  void onRepeatEvent(DateTime timestamp) {
    _engine.runCycle(); // the engine guards re-entrancy internally
  }

  @override
  Future<void> onDestroy(DateTime timestamp, bool isTimeout) async {
    await _sub?.cancel();
    await _engine.dispose();
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
