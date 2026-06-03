import 'dart:convert';
import 'package:http/http.dart' as http;
import 'aircraft.dart';
import '../packet/ble_packet.dart' show bleMaxAircraft;

/// Whether to drop on-ground aircraft from the feed (radar + list). Mirror of the
/// firmware's HIDE_GROUND_AIRCRAFT config.
const bool kHideGroundAircraft = true;

/// Pure: map an airplanes.live /v2/point JSON body to aircraft, nearest-first,
/// capped to 10. Mirrors the field extraction in flight_core.h. When [hideGround]
/// is true, on-ground aircraft are dropped before the sort + cap.
List<Aircraft> parseAircraft(String body, double centerLat, double centerLon,
    {bool hideGround = false}) {
  final dynamic root = json.decode(body);
  final List<dynamic> ac = (root is Map && root['ac'] is List) ? root['ac'] as List : const [];

  final list = <Aircraft>[];
  for (final dynamic item in ac) {
    if (item is! Map) continue;
    final lat = _toDouble(item['lat']);
    final lon = _toDouble(item['lon']);
    if (lat == null || lon == null) continue;

    final altRaw = item['alt_baro'];
    final onGround = altRaw == 'ground';
    if (hideGround && onGround) continue; // drop ground traffic before sort/cap
    final int? altFt = (altRaw is num) ? altRaw.round() : null;
    final int? gsKt = (item['gs'] is num) ? (item['gs'] as num).round() : null;
    final double? track = (item['track'] is num) ? (item['track'] as num).toDouble() : null;
    final int? squawk = (item['squawk'] is String) ? int.tryParse(item['squawk'] as String) : null;
    final String? registration = (item['r'] is String) ? item['r'] as String : null;
    final String hex = (item['hex'] as String?)?.toLowerCase() ?? '';
    final String desc = (item['desc'] as String?)?.trim() ?? '';
    final bool isMilitary = (((item['dbFlags'] as num?)?.toInt()) ?? 0) & 1 != 0;
    final em = item['emergency'] as String?;
    final bool emActive = em != null && em.isNotEmpty && em != 'none';
    final bool isEmergency =
        squawk == 7500 || squawk == 7600 || squawk == 7700 || emActive;

    list.add(Aircraft(
      callsign: (item['flight'] as String?)?.trim() ?? '',
      type: (item['t'] as String?)?.trim() ?? '',
      lat: lat,
      lon: lon,
      altFt: altFt,
      gsKt: gsKt,
      onGround: onGround,
      track: track,
      squawk: squawk,
      registration: registration,
      hex: hex,
      desc: desc,
      isMilitary: isMilitary,
      isEmergency: isEmergency,
    ));
  }

  list.sort((a, b) => haversineKm(centerLat, centerLon, a.lat, a.lon)
      .compareTo(haversineKm(centerLat, centerLon, b.lat, b.lon)));
  if (list.length > bleMaxAircraft) list.removeRange(bleMaxAircraft, list.length);
  return list;
}

double? _toDouble(dynamic v) => (v is num) ? v.toDouble() : null;

/// Split a hexdb.io route ("EGLL-KJFK", possibly multi-leg) into (origin, dest)
/// = (first, last) ICAO. Empty input -> ('', '').
(String, String) parseHexdbRoute(String route) {
  if (route.isEmpty) return ('', '');
  final first = route.indexOf('-');
  if (first < 0) return (route, route);
  return (route.substring(0, first), route.substring(route.lastIndexOf('-') + 1));
}

/// Impure: fetch nearby aircraft from airplanes.live around (lat, lon).
class AirplanesClient {
  final http.Client _http;
  AirplanesClient([http.Client? client]) : _http = client ?? http.Client();

  /// Throws on a transport error or non-200 so the gateway can SKIP the cycle
  /// (vs. a 200 with an empty `ac` list, which legitimately means "no traffic").
  Future<List<Aircraft>> fetchNearby(double lat, double lon, int radiusNm) async {
    final uri = Uri.parse(
        'https://api.airplanes.live/v2/point/${lat.toStringAsFixed(4)}/${lon.toStringAsFixed(4)}/$radiusNm');
    final resp = await _http
        .get(uri, headers: {'User-Agent': 'flight-radar-companion'})
        .timeout(const Duration(seconds: 8));
    if (resp.statusCode != 200) {
      throw Exception('airplanes.live HTTP ${resp.statusCode}');
    }
    return parseAircraft(resp.body, lat, lon, hideGround: kHideGroundAircraft);
  }
}
