import 'dart:async';
import 'package:geolocator/geolocator.dart';
import '../location/location_service.dart' show GpsFix;
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
  GpsFix? _lastFix; // latest position from the keep-alive stream, fed to each cycle

  Future<bool> start() async {
    if (_engine != null) return false; // already running

    final engine = GatewayEngine();
    _engine = engine;
    _engineSub = engine.status.listen((s) {
      if (!_statusController.isClosed) _statusController.add(s);
    });

    try {
      // Keep-alive: a continuous background location stream keeps the Dart event
      // loop running while backgrounded, so the timer keeps firing. Its latest
      // position is also cached and fed to each cycle, so the cycle never blocks
      // on a separate getCurrentPosition (which can stall waiting for a fix).
      // accuracy=medium + distanceFilter=500 m: the stream is only a keep-alive
      // and a rough center for a 50 nm query radius — high accuracy wastes GPS
      // power with no benefit. This is the app's single biggest battery lever.
      _keepAliveSub = Geolocator.getPositionStream(
        locationSettings: AppleSettings(
          accuracy: LocationAccuracy.medium,
          distanceFilter: 500,
          allowBackgroundLocationUpdates: true,
          pauseLocationUpdatesAutomatically: false,
          showBackgroundLocationIndicator: true,
          activityType: ActivityType.other,
        ),
      ).listen(
        (pos) => _lastFix = GpsFix(pos.latitude, pos.longitude),
        onError: (_) {},
      );

      await engine.start();
      // Feed the cached stream position; if none yet, runCycle falls back to a
      // one-shot fix internally.
      _timer = Timer.periodic(
          const Duration(seconds: 15), (_) => engine.runCycle(providedFix: _lastFix));
      return true;
    } catch (_) {
      await stop(); // tear down the partial state
      return false;
    }
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
    _lastFix = null;
    if (!_statusController.isClosed) _statusController.add(const GatewayStatus());
  }

  Future<void> dispose() async {
    await stop();
    await _statusController.close();
  }
}
