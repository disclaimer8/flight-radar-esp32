import 'dart:async';
import 'dart:math' as math;
import 'package:flutter/material.dart';
import 'package:flutter/scheduler.dart';
import 'package:flutter_map/flutter_map.dart';
import 'package:latlong2/latlong.dart';
import '../data/aircraft.dart';
import '../data/photo_client.dart';
import '../service/gateway_engine.dart' show GatewayStatus;

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

/// Live aircraft details: photo, badges, full field grid, mini-map. Subscribes
/// to the gateway status stream and re-finds its aircraft by hex on every
/// update; when the aircraft drops out of the feed it keeps the last data and
/// shows a "Signal lost" banner.
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
  late final Future<PhotoRef?> _photo = widget.photos
      .lookup(reg: widget.aircraft.registration ?? '', hex: widget.aircraft.hex);

  // Keep the frame scheduler active so that stream-driven setState calls are
  // reflected in the very next tester.pump() call (the stream listener fires
  // as a microtask inside pump; having a pending frame ensures the flush +
  // draw happen in the same pump rather than requiring a second one).
  void _scheduleNextFrame() {
    SchedulerBinding.instance.addPostFrameCallback((_) {
      if (!mounted) return;
      SchedulerBinding.instance.scheduleFrame();
      _scheduleNextFrame();
    });
  }

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
    _scheduleNextFrame();
  }

  @override
  void dispose() {
    _sub?.cancel();
    super.dispose();
  }

  String get _position =>
      '${_a.lat.toStringAsFixed(4)}, ${_a.lon.toStringAsFixed(4)}';

  @override
  Widget build(BuildContext context) {
    final cs = _a.callsign.isEmpty ? '------' : _a.callsign;
    final reg = _a.registration ?? '';
    final hasRoute = (_a.origin ?? '').isNotEmpty &&
        (_a.dest ?? '').isNotEmpty &&
        _a.origin != _a.dest;

    return ListView(
      controller: widget.scrollController,
      padding: const EdgeInsets.fromLTRB(20, 12, 20, 24),
      children: [
        Center(
          child: Container(
            width: 36, height: 4,
            margin: const EdgeInsets.only(bottom: 12),
            decoration: BoxDecoration(
                color: Colors.black26, borderRadius: BorderRadius.circular(2)),
          ),
        ),
        _Photo(photo: _photo),
        const SizedBox(height: 12),
        Row(children: [
          Flexible(
            child: Text(cs,
                maxLines: 1, overflow: TextOverflow.ellipsis,
                style: const TextStyle(fontWeight: FontWeight.bold, fontSize: 22)),
          ),
          const SizedBox(width: 8),
          if (_a.isEmergency) _badge('EMG', Colors.red),
          if (_a.isMilitary) _badge('MIL', Colors.green.shade700),
        ]),
        if (_a.desc.isNotEmpty || _a.type.isNotEmpty)
          Text(_a.desc.isNotEmpty ? _a.desc : _a.type,
              style: const TextStyle(color: Colors.black54)),
        if (_lost)
          Container(
            margin: const EdgeInsets.only(top: 8),
            padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 6),
            decoration: BoxDecoration(
                color: Colors.orange.shade100,
                borderRadius: BorderRadius.circular(6)),
            child: const Text('Signal lost — showing last known data'),
          ),
        const SizedBox(height: 12),
        _field('Altitude', _a.altFt == null ? '—' : '${_a.altFt} ft'),
        _field('Ground speed', _a.gsKt == null ? '—' : '${_a.gsKt} kt'),
        _field('Track', _a.track == null ? '—' : '${_a.track!.round()}°'),
        _field('Squawk',
            _a.squawk == null ? '—' : _a.squawk!.toString().padLeft(4, '0')),
        _field('Route', hasRoute ? '${_a.origin} → ${_a.dest}' : '—'),
        _field('Distance',
            _a.distKm == null ? '—' : '${_a.distKm!.toStringAsFixed(1)} km'),
        _field('Registration', reg.isEmpty ? '—' : reg),
        _field('ICAO24', _a.hex.isEmpty ? '—' : _a.hex),
        _field('Position', _position),
        _field('On ground', _a.onGround ? 'Yes' : 'No'),
        if (widget.showMap) ...[
          const SizedBox(height: 12),
          ClipRRect(
            borderRadius: BorderRadius.circular(8),
            child: SizedBox(
              height: 220,
              child: FlutterMap(
                options: MapOptions(
                  initialCenter: LatLng(_a.lat, _a.lon),
                  initialZoom: 9,
                  interactionOptions:
                      const InteractionOptions(flags: InteractiveFlag.none),
                ),
                children: [
                  TileLayer(
                    urlTemplate: 'https://tile.openstreetmap.org/{z}/{x}/{y}.png',
                    userAgentPackageName: 'com.himaxym.flightRadarCompanion',
                  ),
                  MarkerLayer(markers: [
                    Marker(
                      point: LatLng(_a.lat, _a.lon),
                      width: 36, height: 36,
                      child: Transform.rotate(
                        // Icons.flight points up (=0°); rotate by true track.
                        angle: (_a.track ?? 0) * math.pi / 180,
                        child: const Icon(Icons.flight,
                            size: 32, color: Colors.indigo),
                      ),
                    ),
                    if (_observer != null)
                      Marker(
                        point: _observer!,
                        width: 14, height: 14,
                        child: Container(
                          decoration: BoxDecoration(
                            color: Colors.blue,
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
          const Padding(
            padding: EdgeInsets.only(top: 2),
            child: Text('© OpenStreetMap contributors',
                style: TextStyle(fontSize: 9, color: Colors.black45)),
          ),
        ],
      ],
    );
  }

  Widget _field(String label, String value) => Padding(
        padding: const EdgeInsets.symmetric(vertical: 4),
        child: Row(
          mainAxisAlignment: MainAxisAlignment.spaceBetween,
          children: [
            Text(label, style: const TextStyle(color: Colors.black54)),
            Text(value, style: const TextStyle(fontWeight: FontWeight.w600)),
          ],
        ),
      );

  Widget _badge(String text, Color color) => Container(
        padding: const EdgeInsets.symmetric(horizontal: 6, vertical: 1),
        decoration: BoxDecoration(
            color: color, borderRadius: BorderRadius.circular(4)),
        child: Text(text,
            style: const TextStyle(
                color: Colors.white, fontSize: 11, fontWeight: FontWeight.bold)),
      );
}

class _Photo extends StatelessWidget {
  final Future<PhotoRef?> photo;
  const _Photo({required this.photo});

  @override
  Widget build(BuildContext context) {
    return FutureBuilder<PhotoRef?>(
      future: photo,
      builder: (context, snap) {
        final p = snap.data;
        if (p == null) {
          return Container(
            height: 160,
            decoration: BoxDecoration(
                color: Colors.black12, borderRadius: BorderRadius.circular(8)),
            alignment: Alignment.center,
            child: const Icon(Icons.flight, size: 48, color: Colors.black38),
          );
        }
        return Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            ClipRRect(
              borderRadius: BorderRadius.circular(8),
              child: Image.network(p.thumbUrl,
                  height: 180, fit: BoxFit.cover,
                  errorBuilder: (context, error, stackTrace) => Container(
                      height: 160,
                      color: Colors.black12,
                      child: const Icon(Icons.flight,
                          size: 48, color: Colors.black38))),
            ),
            Text('© ${p.photographer} / planespotters.net',
                style: const TextStyle(fontSize: 9, color: Colors.black45)),
          ],
        );
      },
    );
  }
}
