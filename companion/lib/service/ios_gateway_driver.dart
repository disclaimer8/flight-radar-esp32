import 'dart:async';
import 'package:geolocator/geolocator.dart';
import 'gateway_engine.dart';

/// iOS driver. iOS has no foreground service, so the app is kept alive in the
/// background by a continuous location stream (the `location` background mode +
/// "Always" permission). While alive, a periodic timer drives the feed cycle.
/// A fresh engine is created per [start] so the driver is restartable.
class IosGatewayDriver {
  final _statusController = StreamController<GatewayStatus>.broadcast();
  Stream<GatewayStatus> get status => _statusController.stream;

  GatewayEngine? _engine;
  StreamSubscription<GatewayStatus>? _engineSub;
  StreamSubscription<Position>? _keepAliveSub;
  Timer? _timer;

  Future<bool> start() async {
    final engine = GatewayEngine();
    _engine = engine;
    _engineSub = engine.status.listen((s) {
      if (!_statusController.isClosed) _statusController.add(s);
    });

    // Keep-alive: a continuous background location stream keeps the Dart event
    // loop running while backgrounded, so the timer keeps firing. The position
    // values are unused — the engine fetches its own fix per cycle.
    _keepAliveSub = Geolocator.getPositionStream(
      locationSettings: AppleSettings(
        accuracy: LocationAccuracy.high,
        allowBackgroundLocationUpdates: true,
        pauseLocationUpdatesAutomatically: false,
        showBackgroundLocationIndicator: true,
        activityType: ActivityType.other,
      ),
    ).listen((_) {}, onError: (_) {});

    await engine.start();
    _timer = Timer.periodic(const Duration(seconds: 10), (_) => engine.runCycle());
    return true;
  }

  Future<void> stop() async {
    _timer?.cancel();
    _timer = null;
    await _keepAliveSub?.cancel();
    _keepAliveSub = null;
    await _engineSub?.cancel();
    _engineSub = null;
    await _engine?.dispose();
    _engine = null;
    if (!_statusController.isClosed) _statusController.add(const GatewayStatus());
  }

  Future<void> dispose() async {
    await stop();
    await _statusController.close();
  }
}
