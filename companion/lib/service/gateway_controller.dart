import 'dart:async';
import 'package:flutter_foreground_task/flutter_foreground_task.dart';
import 'gateway_task_handler.dart';

/// Snapshot of gateway status shown in the UI.
class GatewayStatus {
  final String ble;
  final int count;
  final String fix;
  const GatewayStatus({this.ble = 'idle', this.count = 0, this.fix = 'no fix'});
}

/// Main-isolate side: initialize, start/stop the service, surface status.
class GatewayController {
  final _statusController = StreamController<GatewayStatus>.broadcast();
  Stream<GatewayStatus> get status => _statusController.stream;
  GatewayStatus _last = const GatewayStatus();

  void init() {
    FlutterForegroundTask.initCommunicationPort();
    FlutterForegroundTask.init(
      androidNotificationOptions: AndroidNotificationOptions(
        channelId: 'flight_radar_gateway',
        channelName: 'Flight Radar Gateway',
        channelDescription: 'Feeds aircraft to the device over BLE.',
        onlyAlertOnce: true,
      ),
      iosNotificationOptions: const IOSNotificationOptions(),
      foregroundTaskOptions: ForegroundTaskOptions(
        eventAction: ForegroundTaskEventAction.repeat(10000),
        autoRunOnBoot: false,
      ),
    );
    FlutterForegroundTask.addTaskDataCallback(_onData);
  }

  void _onData(Object data) {
    if (data is Map) {
      _last = GatewayStatus(
        ble: (data['ble'] as String?) ?? _last.ble,
        count: (data['count'] as int?) ?? _last.count,
        fix: (data['fix'] as String?) ?? _last.fix,
      );
      _statusController.add(_last);
    }
  }

  Future<bool> start() async {
    final result = await FlutterForegroundTask.startService(
      serviceId: 256,
      notificationTitle: 'Feeding Flight Radar',
      notificationText: 'Starting…',
      notificationButtons: const [NotificationButton(id: 'stop', text: 'Stop')],
      callback: startCallback,
    );
    return result is ServiceRequestSuccess;
  }

  Future<void> stop() async {
    await FlutterForegroundTask.stopService();
    _last = const GatewayStatus();
    _statusController.add(_last);
  }

  void dispose() {
    FlutterForegroundTask.removeTaskDataCallback(_onData);
    _statusController.close();
  }
}
