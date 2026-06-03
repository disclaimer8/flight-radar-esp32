import 'package:geolocator/geolocator.dart';

/// A 2D position the gateway centers the packet on.
class GpsFix {
  final double lat;
  final double lon;
  const GpsFix(this.lat, this.lon);
}

/// Abstracts location so the iOS phase can provide a different keep-alive impl.
abstract class LocationService {
  /// Ensure permission; returns false if unavailable/denied.
  Future<bool> ensurePermission();

  /// Latest fix, or null if none yet.
  Future<GpsFix?> currentFix();
}

class GeolocatorLocationService implements LocationService {
  @override
  Future<bool> ensurePermission() async {
    if (!await Geolocator.isLocationServiceEnabled()) return false;
    var p = await Geolocator.checkPermission();
    if (p == LocationPermission.denied) {
      p = await Geolocator.requestPermission();
    }
    return p == LocationPermission.always || p == LocationPermission.whileInUse;
  }

  @override
  Future<GpsFix?> currentFix() async {
    try {
      final pos = await Geolocator.getCurrentPosition(
        locationSettings: const LocationSettings(
          accuracy: LocationAccuracy.high,
          timeLimit: Duration(seconds: 10),
        ),
      );
      return GpsFix(pos.latitude, pos.longitude);
    } catch (_) {
      return null;
    }
  }
}
