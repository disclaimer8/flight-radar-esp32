import 'dart:math' as math;

/// One aircraft. `altFt` / `gsKt` are null when the source value is missing
/// (they map to the wire packet's ALT_VALID / GS_VALID flags being clear).
class Aircraft {
  final String callsign;
  final String type;
  final double lat;
  final double lon;
  final int? altFt;
  final int? gsKt;
  final bool onGround;
  final double? track; // true track degrees; null if missing
  final int? squawk;   // transponder code; null if missing
  final String? registration; // tail number; null if missing
  final String? origin; // origin ICAO; enriched later, null until then
  final String? dest;   // destination ICAO; enriched later, null until then

  const Aircraft({
    required this.callsign,
    required this.type,
    required this.lat,
    required this.lon,
    required this.altFt,
    required this.gsKt,
    required this.onGround,
    this.track,
    this.squawk,
    this.registration,
    this.origin,
    this.dest,
  });

  Aircraft copyWith({String? origin, String? dest}) => Aircraft(
        callsign: callsign, type: type, lat: lat, lon: lon, altFt: altFt,
        gsKt: gsKt, onGround: onGround, track: track, squawk: squawk,
        registration: registration, origin: origin ?? this.origin, dest: dest ?? this.dest,
      );
}

/// Great-circle distance in kilometres. Mirror of `flight_core.h` haversineKm.
double haversineKm(double lat1, double lon1, double lat2, double lon2) {
  const r = 6371.0;
  final dLat = _rad(lat2 - lat1);
  final dLon = _rad(lon2 - lon1);
  final a = math.sin(dLat / 2) * math.sin(dLat / 2) +
      math.cos(_rad(lat1)) * math.cos(_rad(lat2)) *
          math.sin(dLon / 2) * math.sin(dLon / 2);
  return r * 2 * math.atan2(math.sqrt(a), math.sqrt(1 - a));
}

double _rad(double deg) => deg * math.pi / 180.0;
