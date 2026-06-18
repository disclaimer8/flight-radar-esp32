import 'dart:async';
import 'dart:math' as math;
import 'package:flutter/material.dart';
import 'package:flutter_map/flutter_map.dart';
import 'package:flutter_map_cancellable_tile_provider/flutter_map_cancellable_tile_provider.dart';
import 'package:latlong2/latlong.dart';
import '../data/aircraft.dart';
import '../data/photo_client.dart';
import '../service/gateway_engine.dart' show GatewayStatus;
import '../theme/app_theme.dart';
import 'catch_card.dart';

/// Parse the observer position out of GatewayStatus.fix ("51.5074, -0.1278").
/// Returns null for "no fix" or anything unparseable.
LatLng? parseFix(String fix) {
  final parts = fix.split(',');
  if (parts.length != 2) return null;
  final lat = double.tryParse(parts[0].trim());
  final lon = double.tryParse(parts[1].trim());
  if (lat == null || lon == null) return null;
  return LatLng(lat, lon);
}

/// Open the live detail sheet for [aircraft].
Future<void> showAircraftDetail(BuildContext context, Aircraft aircraft,
    PhotoClient photos, Stream<GatewayStatus> status) {
  return showModalBottomSheet(
    context: context,
    isScrollControlled: true,
    builder: (_) => DraggableScrollableSheet(
      expand: false,
      initialChildSize: 0.85,
      minChildSize: 0.4,
      maxChildSize: 0.95,
      builder: (context, scroll) => AircraftDetailSheet(
        aircraft: aircraft,
        photos: photos,
        status: status,
        scrollController: scroll,
      ),
    ),
  );
}

/// Live aircraft details: photo, badges, grouped field grid, mini-map, share.
/// Subscribes to the gateway status stream and re-finds its aircraft by hex on
/// every update; when it drops out of the feed it keeps the last data and shows
/// a "Signal lost" banner.
class AircraftDetailSheet extends StatefulWidget {
  final Aircraft aircraft;
  final PhotoClient photos;
  final Stream<GatewayStatus> status;
  final ScrollController? scrollController;
  final bool showMap; // false in widget tests (tiles need the network)
  const AircraftDetailSheet({
    super.key,
    required this.aircraft,
    required this.photos,
    required this.status,
    this.scrollController,
    this.showMap = true,
  });

  @override
  State<AircraftDetailSheet> createState() => _AircraftDetailSheetState();
}

class _AircraftDetailSheetState extends State<AircraftDetailSheet> {
  late Aircraft _a = widget.aircraft;
  LatLng? _observer;
  bool _lost = false;
  StreamSubscription<GatewayStatus>? _sub;
  PhotoRef? _photoRef;
  late final Future<PhotoRef?> _photo = widget.photos
      .lookup(reg: widget.aircraft.registration ?? '', hex: widget.aircraft.hex)
      .then((p) {
    _photoRef = p;
    return p;
  });

  @override
  void initState() {
    super.initState();
    _sub = widget.status.listen((s) {
      Aircraft? match;
      for (final x in s.aircraft) {
        final byHex = widget.aircraft.hex.isNotEmpty && x.hex == widget.aircraft.hex;
        final byCallsign = widget.aircraft.hex.isEmpty &&
            x.callsign == widget.aircraft.callsign;
        if (byHex || byCallsign) {
          match = x;
          break;
        }
      }
      if (!mounted) return;
      setState(() {
        _observer = parseFix(s.fix) ?? _observer;
        if (match != null) {
          _a = match;
          _lost = false;
        } else {
          _lost = true;
        }
      });
    });
  }

  @override
  void dispose() {
    _sub?.cancel();
    super.dispose();
  }

  String get _position =>
      '${_a.lat.toStringAsFixed(4)}, ${_a.lon.toStringAsFixed(4)}';

  bool get _hasRoute =>
      (_a.origin ?? '').isNotEmpty &&
      (_a.dest ?? '').isNotEmpty &&
      _a.origin != _a.dest;

  @override
  Widget build(BuildContext context) {
    final cs = _a.callsign.isEmpty ? '------' : _a.callsign;
    final reg = _a.registration ?? '';
    final ac = Theme.of(context).extension<AppColors>()!;

    return ListView(
      controller: widget.scrollController,
      padding: const EdgeInsets.fromLTRB(20, 12, 20, 24),
      children: [
        Center(
          child: Container(
            width: 36, height: 4,
            margin: const EdgeInsets.only(bottom: 12),
            decoration: BoxDecoration(
                color: ac.muted, borderRadius: BorderRadius.circular(2)),
          ),
        ),
        _Photo(photo: _photo),
        const SizedBox(height: 12),
        Row(children: [
          Expanded(
            child: Text(cs,
                maxLines: 1, overflow: TextOverflow.ellipsis,
                style: AppTheme.callsign(context, size: 24)),
          ),
          if (_a.isEmergency) _badge('EMG', Icons.warning_amber_rounded, ac.emg),
          if (_a.isMilitary) _badge('MIL', Icons.shield_rounded, ac.mil),
          IconButton(
            tooltip: 'Share catch',
            onPressed: () => shareCatch(context, _a, _photoRef, _observer),
            icon: const Icon(Icons.ios_share),
          ),
        ]),
        if (_a.desc.isNotEmpty || _a.type.isNotEmpty)
          Text(_a.desc.isNotEmpty ? _a.desc : _a.type,
              style: TextStyle(color: ac.muted)),
        if (_lost)
          Container(
            margin: const EdgeInsets.only(top: 8),
            padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 6),
            decoration: BoxDecoration(
                color: ac.emg.withValues(alpha: 0.15),
                borderRadius: BorderRadius.circular(8)),
            child: Row(children: [
              Icon(Icons.signal_wifi_off, size: 16, color: ac.emg),
              const SizedBox(width: 8),
              const Expanded(child: Text('Signal lost — showing last known data')),
            ]),
          ),
        const SizedBox(height: 16),
        _section(context, 'Flight'),
        _field('Route', _hasRoute ? '${_a.origin} → ${_a.dest}' : '—'),
        _field('Altitude', _a.altFt == null ? '—' : '${_a.altFt} ft'),
        _field('Ground speed', _a.gsKt == null ? '—' : '${_a.gsKt} kt'),
        _field('Track', _a.track == null ? '—' : '${_a.track!.round()}°'),
        _field('Squawk',
            _a.squawk == null ? '—' : _a.squawk!.toString().padLeft(4, '0')),
        const SizedBox(height: 12),
        _section(context, 'Aircraft'),
        _field('Type', _a.type.isEmpty ? '—' : _a.type),
        _field('Registration', reg.isEmpty ? '—' : reg),
        _field('ICAO24', _a.hex.isEmpty ? '—' : _a.hex),
        const SizedBox(height: 12),
        _section(context, 'Position'),
        _field('Distance',
            _a.distKm == null ? '—' : '${_a.distKm!.toStringAsFixed(1)} km'),
        _field('Coordinates', _position),
        _field('On ground', _a.onGround ? 'Yes' : 'No'),
        if (widget.showMap) ...[
          const SizedBox(height: 12),
          ClipRRect(
            borderRadius: BorderRadius.circular(12),
            child: SizedBox(
              height: 220,
              child: FlutterMap(
                options: MapOptions(
                  initialCenter: LatLng(_a.lat, _a.lon),
                  initialZoom: 9,
                  interactionOptions: const InteractionOptions(
                      flags: InteractiveFlag.pinchZoom | InteractiveFlag.drag),
                ),
                children: [
                  TileLayer(
                    urlTemplate: 'https://tile.openstreetmap.org/{z}/{x}/{y}.png',
                    userAgentPackageName: 'com.himaxym.flightRadarCompanion',
                    tileProvider: CancellableNetworkTileProvider(),
                  ),
                  if (_observer != null)
                    PolylineLayer(polylines: [
                      Polyline(
                        points: [_observer!, LatLng(_a.lat, _a.lon)],
                        strokeWidth: 2,
                        color: ac.accent.withValues(alpha: 0.7),
                      ),
                    ]),
                  MarkerLayer(markers: [
                    Marker(
                      point: LatLng(_a.lat, _a.lon),
                      width: 36, height: 36,
                      child: Transform.rotate(
                        // Icons.flight points up (=0°); rotate by true track.
                        angle: (_a.track ?? 0) * math.pi / 180,
                        child: Icon(Icons.flight, size: 32, color: ac.accent),
                      ),
                    ),
                    if (_observer != null)
                      Marker(
                        point: _observer!,
                        width: 14, height: 14,
                        child: Container(
                          decoration: BoxDecoration(
                            color: ac.accent,
                            shape: BoxShape.circle,
                            border: Border.all(color: Colors.white, width: 2),
                          ),
                        ),
                      ),
                  ]),
                ],
              ),
            ),
          ),
          Padding(
            padding: const EdgeInsets.only(top: 4),
            child: Text('© OpenStreetMap contributors',
                style: TextStyle(fontSize: 11, color: ac.muted)),
          ),
        ],
      ],
    );
  }

  Widget _section(BuildContext context, String title) => Padding(
        padding: const EdgeInsets.only(bottom: 4),
        child: Text(title.toUpperCase(),
            style: TextStyle(
                fontSize: 11,
                fontWeight: FontWeight.w700,
                letterSpacing: 1,
                color: Theme.of(context).colorScheme.primary)),
      );

  Widget _field(String label, String value) => Builder(builder: (context) {
        final ac = Theme.of(context).extension<AppColors>()!;
        return Padding(
          padding: const EdgeInsets.symmetric(vertical: 4),
          child: Row(
            mainAxisAlignment: MainAxisAlignment.spaceBetween,
            children: [
              Text(label, style: TextStyle(color: ac.muted)),
              Flexible(
                child: Text(value,
                    textAlign: TextAlign.right,
                    style: const TextStyle(fontWeight: FontWeight.w600)),
              ),
            ],
          ),
        );
      });

  Widget _badge(String text, IconData icon, Color color) => Padding(
        padding: const EdgeInsets.only(left: 6),
        child: Semantics(
          label: text == 'EMG' ? 'Emergency' : 'Military',
          child: Container(
            padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 4),
            decoration: BoxDecoration(
                color: color, borderRadius: BorderRadius.circular(6)),
            child: Row(mainAxisSize: MainAxisSize.min, children: [
              Icon(icon, size: 13, color: Colors.white),
              const SizedBox(width: 4),
              Text(text,
                  style: const TextStyle(
                      color: Colors.white, fontSize: 11, fontWeight: FontWeight.w700)),
            ]),
          ),
        ),
      );
}

class _Photo extends StatelessWidget {
  final Future<PhotoRef?> photo;
  const _Photo({required this.photo});

  @override
  Widget build(BuildContext context) {
    final ac = Theme.of(context).extension<AppColors>()!;
    final fill = Theme.of(context).colorScheme.surfaceContainerHighest;
    Widget placeholder() => Container(
          height: 180,
          decoration: BoxDecoration(
              color: fill, borderRadius: BorderRadius.circular(12)),
          alignment: Alignment.center,
          child: Icon(Icons.flight, size: 48, color: ac.muted),
        );
    return FutureBuilder<PhotoRef?>(
      future: photo,
      builder: (context, snap) {
        if (snap.connectionState != ConnectionState.done) return placeholder();
        final p = snap.data;
        if (p == null) return placeholder();
        return Stack(children: [
          ClipRRect(
            borderRadius: BorderRadius.circular(12),
            child: Image.network(p.thumbUrl,
                height: 200, width: double.infinity, fit: BoxFit.cover,
                cacheWidth: 720, // bound the decode (thumbnails can be 1024px+)
                errorBuilder: (_, _, _) => placeholder()),
          ),
          Positioned(
            left: 0, right: 0, bottom: 0,
            child: Container(
              padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 4),
              decoration: const BoxDecoration(
                borderRadius: BorderRadius.vertical(bottom: Radius.circular(12)),
                gradient: LinearGradient(
                  begin: Alignment.bottomCenter, end: Alignment.topCenter,
                  colors: [Colors.black54, Colors.transparent],
                ),
              ),
              child: Text('© ${p.photographer} / planespotters.net',
                  style: const TextStyle(fontSize: 11, color: Colors.white)),
            ),
          ),
        ]);
      },
    );
  }
}
