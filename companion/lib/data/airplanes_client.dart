import 'dart:convert';
import 'package:http/http.dart' as http;
import 'aircraft.dart';
import '../packet/ble_packet.dart' show bleMaxAircraft;

/// Pure: map an airplanes.live /v2/point JSON body to aircraft, nearest-first,
/// capped to 16. Mirrors the field extraction in flight_core.h.
List<Aircraft> parseAircraft(String body, double centerLat, double centerLon) {
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
    final int? altFt = (altRaw is num) ? altRaw.round() : null;
    final int? gsKt = (item['gs'] is num) ? (item['gs'] as num).round() : null;

    list.add(Aircraft(
      callsign: (item['flight'] as String?)?.trim() ?? '',
      type: (item['t'] as String?)?.trim() ?? '',
      lat: lat,
      lon: lon,
      altFt: altFt,
      gsKt: gsKt,
      onGround: onGround,
    ));
  }

  list.sort((a, b) => haversineKm(centerLat, centerLon, a.lat, a.lon)
      .compareTo(haversineKm(centerLat, centerLon, b.lat, b.lon)));
  if (list.length > bleMaxAircraft) list.removeRange(bleMaxAircraft, list.length);
  return list;
}

double? _toDouble(dynamic v) => (v is num) ? v.toDouble() : null;

/// Impure: fetch nearby aircraft from airplanes.live around (lat, lon).
class AirplanesClient {
  final http.Client _http;
  AirplanesClient([http.Client? client]) : _http = client ?? http.Client();

  /// Throws on a transport error or non-200 so the gateway can SKIP the cycle
  /// (vs. a 200 with an empty `ac` list, which legitimately means "no traffic").
  Future<List<Aircraft>> fetchNearby(double lat, double lon, int radiusNm) async {
    final uri = Uri.parse(
        'https://api.airplanes.live/v2/point/${lat.toStringAsFixed(4)}/${lon.toStringAsFixed(4)}/$radiusNm');
    final resp = await _http.get(uri, headers: {'User-Agent': 'flight-radar-companion'});
    if (resp.statusCode != 200) {
      throw Exception('airplanes.live HTTP ${resp.statusCode}');
    }
    return parseAircraft(resp.body, lat, lon);
  }
}
