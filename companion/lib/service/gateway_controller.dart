import 'dart:async';
import 'dart:io' show Platform;
import 'package:flutter_foreground_task/flutter_foreground_task.dart';
import '../data/aircraft.dart';
import 'gateway_engine.dart' show GatewayStatus;
import 'gateway_task_handler.dart';
import 'ios_gateway_driver.dart';

/// Main-isolate side: initialize, start/stop, and surface status. Branches by
/// platform — Android uses a foreground service; iOS runs the engine in-process
/// via [IosGatewayDriver].
class GatewayController {
  final _statusController = StreamController<GatewayStatus>.broadcast();
  Stream<GatewayStatus> get status => _statusController.stream;
  GatewayStatus _last = const GatewayStatus();

  IosGatewayDriver? _ios;
  StreamSubscription<GatewayStatus>? _iosSub;

  void init() {
    if (Platform.isIOS) {
      final ios = IosGatewayDriver();
      _ios = ios;
      _iosSub = ios.status.listen((s) {
        _last = s;
        _statusController.add(s);
      });
      return;
    }
    // Android: foreground service.
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
      List<Aircraft> aircraft = _last.aircraft;
      if (data['aircraft'] is List) {
        aircraft = (data['aircraft'] as List)
            .whereType<Map>()
            .map((m) => Aircraft.fromJson(Map<String, dynamic>.from(m)))
            .toList();
      }
      _last = GatewayStatus(
        ble: (data['ble'] as String?) ?? _last.ble,
        count: (data['count'] as int?) ?? _last.count,
        fix: (data['fix'] as String?) ?? _last.fix,
        aircraft: aircraft,
      );
      _statusController.add(_last);
    }
  }

  Future<bool> start() async {
    if (Platform.isIOS) {
      return await _ios?.start() ?? false;
    }
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
    if (Platform.isIOS) {
      await _ios?.stop();
      return;
    }
    await FlutterForegroundTask.stopService();
    _last = const GatewayStatus();
    _statusController.add(_last);
  }

  void dispose() {
    if (Platform.isIOS) {
      _iosSub?.cancel();
      _ios?.dispose();
    } else {
      FlutterForegroundTask.removeTaskDataCallback(_onData);
    }
    _statusController.close();
  }
}
