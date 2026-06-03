import 'dart:convert';
import 'package:http/http.dart' as http;
import 'airplanes_client.dart' show parseHexdbRoute;

/// Looks up a flight's route from hexdb.io and caches it per callsign
/// (a route is stable within a flight).
class RouteClient {
  final http.Client _http;
  final Map<String, (String, String)> _cache = {};
  RouteClient([http.Client? client]) : _http = client ?? http.Client();

  /// (origin, dest) ICAO for a callsign; ('','') on unknown/error. Cached
  /// (including empties, so an unknown callsign is looked up at most once).
  Future<(String, String)> lookup(String callsign) async {
    if (callsign.isEmpty) return ('', '');
    final hit = _cache[callsign];
    if (hit != null) return hit;
    (String, String) result = ('', '');
    try {
      final resp = await _http.get(
        Uri.parse('https://hexdb.io/api/v1/route/icao/$callsign'),
        headers: {'User-Agent': 'flight-radar-companion'},
      );
      if (resp.statusCode == 200) {
        final m = json.decode(resp.body);
        if (m is Map && m['route'] is String) result = parseHexdbRoute(m['route'] as String);
      }
    } catch (_) {/* leave empty */}
    _cache[callsign] = result;
    return result;
  }
}
