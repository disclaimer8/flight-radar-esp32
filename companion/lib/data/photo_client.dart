import 'dart:convert';
import 'package:http/http.dart' as http;

/// A single aircraft photo reference from planespotters.net.
class PhotoRef {
  final String thumbUrl;
  final String photographer;
  final String link;
  const PhotoRef(this.thumbUrl, this.photographer, this.link);
}

/// Fetches aircraft photos from the planespotters.net public API, by registration
/// then hex, caching results (including misses) by lookup key. Photos are fetched
/// only from the UI (foreground); never from the background feed cycle.
class PhotoClient {
  // planespotters rejects generic User-Agents (HTTP 403) — must be descriptive
  // with a contact URL.
  static const _ua =
      'flight-radar-esp32-companion/1.0 (+https://github.com/disclaimer8/flight-radar-esp32)';
  static const _maxCache = 256; // bound growth over a long session
  final http.Client _http;
  final Map<String, PhotoRef?> _cache = {};
  PhotoClient([http.Client? client]) : _http = client ?? http.Client();

  /// Try [reg] first, then [hex]; the first hit wins, else null.
  Future<PhotoRef?> lookup({required String reg, required String hex}) async {
    if (reg.isNotEmpty) {
      final r = await _byKey('reg', reg);
      if (r != null) return r;
    }
    if (hex.isNotEmpty) {
      final r = await _byKey('hex', hex);
      if (r != null) return r;
    }
    return null;
  }

  Future<PhotoRef?> _byKey(String kind, String value) async {
    final cacheKey = '$kind/$value';
    if (_cache.containsKey(cacheKey)) return _cache[cacheKey];
    PhotoRef? result;
    try {
      final resp = await _http.get(
        Uri.parse('https://api.planespotters.net/pub/photos/$kind/$value'),
        headers: {'User-Agent': _ua},
      ).timeout(const Duration(seconds: 8));
      if (resp.statusCode == 200) {
        final m = json.decode(resp.body);
        if (m is Map && m['photos'] is List && (m['photos'] as List).isNotEmpty) {
          final p = (m['photos'] as List).first as Map;
          final thumb = (p['thumbnail_large'] ?? p['thumbnail']) as Map?;
          final src = thumb?['src'] as String?;
          if (src != null) {
            result = PhotoRef(src, (p['photographer'] as String?) ?? '',
                (p['link'] as String?) ?? '');
          }
        }
      }
    } catch (_) {/* leave as a miss */}
    if (_cache.length >= _maxCache) _cache.clear();
    _cache[cacheKey] = result;
    return result;
  }
}
