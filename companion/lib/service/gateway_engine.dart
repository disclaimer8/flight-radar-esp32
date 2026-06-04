import 'dart:async';
import '../ble/ble_manager.dart';
import '../data/aircraft.dart';
import '../data/airplanes_client.dart';
import '../data/route_client.dart';
import '../location/location_service.dart';
import '../packet/ble_packet.dart';
import 'alerts.dart';
import 'notification_service.dart';

const int kRadiusNm = 50;

/// Snapshot of gateway status for the UI.
class GatewayStatus {
  final String ble;
  final int count;
  final String fix;
  final List<Aircraft> aircraft;
  const GatewayStatus(
      {this.ble = 'idle', this.count = 0, this.fix = 'no fix', this.aircraft = const []});
}

/// Platform-agnostic feed engine: owns the BLE link, location, and the
/// airplanes.live client, and runs one cycle (GPS -> fetch -> encode -> BLE write)
/// per [runCycle] call. Drivers (Android foreground-service, iOS location-stream)
/// call start/runCycle/stop and forward [status]. No flutter_foreground_task dep.
class GatewayEngine {
  final BleManager _ble = BleManager();
  final LocationService _location = GeolocatorLocationService();
  final AirplanesClient _client = AirplanesClient();
  final RouteClient _routes = RouteClient();

  final _statusController = StreamController<GatewayStatus>.broadcast();
  Stream<GatewayStatus> get status => _statusController.stream;

  String _bleState = 'idle';
  int _count = 0;
  String _fix = 'no fix';
  StreamSubscription<BleStatus>? _bleSub;
  bool _busy = false;
  final NotificationService _notify = NotificationService();
  List<Aircraft> _lastAircraft = const [];
  Set<String> _alerted = {};

  void _emit() {
    if (!_statusController.isClosed) {
      _statusController.add(GatewayStatus(
          ble: _bleState, count: _count, fix: _fix, aircraft: _lastAircraft));
    }
  }

  /// Connect BLE and begin tracking its status. Call once before [runCycle].
  Future<void> start() async {
    _bleSub = _ble.status.listen((s) {
      _bleState = s.name;
      _emit();
    });
    await _notify.init();
    await _ble.start();
  }

  /// One feed cycle. Re-entrancy-guarded: a tick is skipped if the previous
  /// cycle is still running (a cycle can exceed the driver's interval).
  ///
  /// [providedFix] lets a driver supply a center it already has (iOS feeds the
  /// latest position from its keep-alive stream), avoiding a per-cycle
  /// `getCurrentPosition` that can stall the cycle. When null, the engine
  /// fetches its own one-shot fix (Android).
  Future<void> runCycle({GpsFix? providedFix}) async {
    if (_busy) return;
    _busy = true;
    try {
      await _cycle(providedFix);
    } finally {
      _busy = false;
    }
  }

  Future<void> _cycle(GpsFix? providedFix) async {
    final fix = providedFix ?? await _location.currentFix();
    if (fix == null) {
      _fix = 'no fix';
      _emit();
      return;
    }
    _fix = '${fix.lat.toStringAsFixed(4)}, ${fix.lon.toStringAsFixed(4)}';
    try {
      final aircraft = await _client.fetchNearby(fix.lat, fix.lon, kRadiusNm);
      // Parallel route lookups: all callsigns are requested concurrently instead
      // of awaiting each serially (cold cache with N aircraft = N×RTT → 1×RTT).
      // RouteClient caches by callsign; duplicate callsigns in one batch may
      // double-fetch on a cold cache (rare), which is acceptable.
      final routes = await Future.wait(
        aircraft.map((a) => _routes.lookup(a.callsign)),
      );
      final enriched = <Aircraft>[];
      for (var i = 0; i < aircraft.length; i++) {
        final a = aircraft[i];
        final (o, d) = routes[i];
        // Reuse distKm already computed by AirplanesClient (single-pass haversine).
        // Fall back to computing it here if the client didn't set it (foreign data).
        final dist = a.distKm ?? haversineKm(fix.lat, fix.lon, a.lat, a.lon);
        var e = a.copyWith(distKm: dist);
        if (o.isNotEmpty) e = e.copyWith(origin: o, dest: d);
        enriched.add(e);
      }
      _lastAircraft = enriched;

      // Emergency/military alerts (run regardless of foreground/background). A
      // notification failure must never abort the feed.
      final pass = computeNewAlerts(enriched, _alerted);
      _alerted = pass.alerted;
      for (final a in pass.newAlerts) {
        try {
          await _notify.show(a.hex.hashCode & 0x7fffffff, alertTitle(a), alertBody(a));
        } catch (_) {/* ignore */}
      }

      final packet = encodePacket(fix.lat, fix.lon, enriched);
      final ok = await _ble.sendPacket(packet);
      if (ok) _count = aircraft.length;
    } catch (_) {
      // offline / fetch failed: skip the send so we don't refresh the device's
      // freshness window (let it fall back to NO LINK if truly offline).
    }
    _emit();
  }

  Future<void> stop() async {
    await _bleSub?.cancel();
    await _ble.stop();
    await _ble.dispose();
    _bleState = 'idle';
    _emit();
  }

  Future<void> dispose() async {
    await stop();
    await _statusController.close();
  }
}
