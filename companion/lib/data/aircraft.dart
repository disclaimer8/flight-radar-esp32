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
  final String hex;     // ICAO24 lowercase; "" if missing (photo fallback + alert id)
  final String desc;    // full type description; "" if missing (card subtitle)
  final bool isMilitary;  // dbFlags bit 0
  final bool isEmergency; // emergency squawk or emergency status field
  final double? distKm;   // distance from the GPS center; set by the engine

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
    this.hex = '',
    this.desc = '',
    this.isMilitary = false,
    this.isEmergency = false,
    this.distKm,
  });

  Aircraft copyWith({String? origin, String? dest, double? distKm}) => Aircraft(
        callsign: callsign, type: type, lat: lat, lon: lon, altFt: altFt,
        gsKt: gsKt, onGround: onGround, track: track, squawk: squawk,
        registration: registration, origin: origin ?? this.origin, dest: dest ?? this.dest,
        hex: hex, desc: desc, isMilitary: isMilitary, isEmergency: isEmergency,
        distKm: distKm ?? this.distKm,
      );

  Map<String, dynamic> toJson() => {
        'callsign': callsign, 'type': type, 'lat': lat, 'lon': lon,
        'altFt': altFt, 'gsKt': gsKt, 'onGround': onGround,
        'track': track, 'squawk': squawk, 'registration': registration,
        'origin': origin, 'dest': dest, 'hex': hex, 'desc': desc,
        'isMilitary': isMilitary, 'isEmergency': isEmergency, 'distKm': distKm,
      };

  factory Aircraft.fromJson(Map<String, dynamic> m) => Aircraft(
        callsign: m['callsign'] as String? ?? '',
        type: m['type'] as String? ?? '',
        lat: (m['lat'] as num?)?.toDouble() ?? 0,
        lon: (m['lon'] as num?)?.toDouble() ?? 0,
        altFt: (m['altFt'] as num?)?.toInt(),
        gsKt: (m['gsKt'] as num?)?.toInt(),
        onGround: m['onGround'] as bool? ?? false,
        track: (m['track'] as num?)?.toDouble(),
        squawk: (m['squawk'] as num?)?.toInt(),
        registration: m['registration'] as String?,
        origin: m['origin'] as String?,
        dest: m['dest'] as String?,
        hex: m['hex'] as String? ?? '',
        desc: m['desc'] as String? ?? '',
        isMilitary: m['isMilitary'] as bool? ?? false,
        isEmergency: m['isEmergency'] as bool? ?? false,
        distKm: (m['distKm'] as num?)?.toDouble(),
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
