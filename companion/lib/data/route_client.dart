import 'dart:async';
import 'dart:convert';
import 'package:http/http.dart' as http;
import 'airplanes_client.dart' show parseHexdbRoute;

/// Looks up a flight's route from hexdb.io and caches it per callsign
/// (a route is stable within a flight). Route enrichment is cosmetic, so the
/// feed cycle never blocks on it: it sends with [peek]ed (cached) routes and
/// [warm]s the cache in the background for next cycle.
class RouteClient {
  static const _maxCache = 256; // bound growth (callsigns rotate constantly)
  final http.Client _http;
  final Map<String, (String, String)> _cache = {};
  RouteClient([http.Client? client]) : _http = client ?? http.Client();

  /// Cached route for a callsign, or null if not looked up yet (no network).
  (String, String)? peek(String callsign) =>
      callsign.isEmpty ? ('', '') : _cache[callsign];

  /// Fire-and-forget: look up any callsigns not already cached, so the next
  /// cycle's [peek] hits. Bounded concurrency timeout; never throws.
  Future<void> warm(Iterable<String> callsigns) async {
    final pending =
        callsigns.where((c) => c.isNotEmpty && !_cache.containsKey(c)).toSet();
    if (pending.isEmpty) return;
    await Future.wait(pending.map(lookup)).timeout(
      const Duration(seconds: 6),
      onTimeout: () => const [],
    );
  }

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
      ).timeout(const Duration(seconds: 4));
      if (resp.statusCode == 200) {
        final m = json.decode(resp.body);
        if (m is Map && m['route'] is String) result = parseHexdbRoute(m['route'] as String);
      }
    } catch (_) {/* leave empty */}
    if (_cache.length >= _maxCache) _cache.clear();
    _cache[callsign] = result;
    return result;
  }
}
