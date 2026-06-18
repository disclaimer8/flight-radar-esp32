import 'package:flutter/material.dart';
import '../data/aircraft.dart';
import '../data/photo_client.dart';
import '../theme/app_theme.dart';

/// One aircraft row: photo (left), callsign + glyph badges, type, route, reg,
/// and a right-anchored distance + compass-bearing chip (where to look up).
/// The photo is looked up lazily from [photos] (foreground only).
class AircraftCard extends StatelessWidget {
  final Aircraft aircraft;
  final PhotoClient photos;
  final VoidCallback? onTap;
  final double? observerLat;
  final double? observerLon;
  const AircraftCard({
    super.key,
    required this.aircraft,
    required this.photos,
    this.onTap,
    this.observerLat,
    this.observerLon,
  });

  bool get _hasRoute =>
      (aircraft.origin ?? '').isNotEmpty &&
      (aircraft.dest ?? '').isNotEmpty &&
      aircraft.origin != aircraft.dest;

  String? get _bearingLabel {
    if (observerLat == null || observerLon == null) return null;
    final b = bearingDeg(observerLat!, observerLon!, aircraft.lat, aircraft.lon);
    return compass8(b);
  }

  @override
  Widget build(BuildContext context) {
    final cs = aircraft.callsign.isEmpty ? '------' : aircraft.callsign;
    final subtitle = aircraft.desc.isNotEmpty ? aircraft.desc : aircraft.type;
    final reg = aircraft.registration ?? '';
    final ac = Theme.of(context).extension<AppColors>()!;
    final scheme = Theme.of(context).colorScheme;

    return Card(
      margin: const EdgeInsets.symmetric(horizontal: 12, vertical: 5),
      child: InkWell(
        onTap: onTap,
        child: Padding(
          padding: const EdgeInsets.all(10),
          child: Row(
            crossAxisAlignment: CrossAxisAlignment.center,
            children: [
              _PhotoBox(reg: reg, hex: aircraft.hex, photos: photos),
              const SizedBox(width: 12),
              Expanded(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  mainAxisSize: MainAxisSize.min,
                  children: [
                    Row(children: [
                      Flexible(
                        child: Text(cs,
                            maxLines: 1,
                            overflow: TextOverflow.ellipsis,
                            style: AppTheme.callsign(context, size: 18)),
                      ),
                      const SizedBox(width: 8),
                      if (aircraft.isEmergency)
                        _Badge(label: 'EMG', icon: Icons.warning_amber_rounded, color: ac.emg),
                      if (aircraft.isMilitary)
                        _Badge(label: 'MIL', icon: Icons.shield_rounded, color: ac.mil),
                    ]),
                    if (subtitle.isNotEmpty)
                      Padding(
                        padding: const EdgeInsets.only(top: 2),
                        child: Text(subtitle,
                            maxLines: 1,
                            overflow: TextOverflow.ellipsis,
                            style: TextStyle(fontSize: 13, color: ac.muted)),
                      ),
                    if (_hasRoute)
                      Padding(
                        padding: const EdgeInsets.only(top: 2),
                        child: Text.rich(TextSpan(children: [
                          TextSpan(text: '${aircraft.origin} '),
                          TextSpan(text: '→', style: TextStyle(color: ac.accent)),
                          TextSpan(text: ' ${aircraft.dest}'),
                        ]), maxLines: 1, overflow: TextOverflow.ellipsis,
                            style: const TextStyle(fontSize: 14)),
                      ),
                    if (reg.isNotEmpty)
                      Padding(
                        padding: const EdgeInsets.only(top: 2),
                        child: Text(reg, style: TextStyle(fontSize: 12, color: ac.muted)),
                      ),
                  ],
                ),
              ),
              const SizedBox(width: 8),
              if (aircraft.distKm != null)
                _DistanceChip(
                  km: aircraft.distKm!,
                  bearing: _bearingLabel,
                  accent: scheme.primary,
                  muted: ac.muted,
                ),
            ],
          ),
        ),
      ),
    );
  }
}

class _DistanceChip extends StatelessWidget {
  final double km;
  final String? bearing;
  final Color accent;
  final Color muted;
  const _DistanceChip(
      {required this.km, required this.bearing, required this.accent, required this.muted});

  @override
  Widget build(BuildContext context) {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.end,
      mainAxisSize: MainAxisSize.min,
      children: [
        Text.rich(TextSpan(children: [
          TextSpan(text: '${km.round()}',
              style: TextStyle(fontSize: 22, fontWeight: FontWeight.w700, color: accent)),
          TextSpan(text: ' km', style: TextStyle(fontSize: 12, color: muted)),
        ])),
        if (bearing != null)
          Text(bearing!,
              style: TextStyle(fontSize: 12, color: muted, fontWeight: FontWeight.w600)),
      ],
    );
  }
}

class _Badge extends StatelessWidget {
  final String label;
  final IconData icon;
  final Color color;
  const _Badge({required this.label, required this.icon, required this.color});

  @override
  Widget build(BuildContext context) {
    // Glyph + label so meaning never depends on color alone (color-blind safe).
    return Semantics(
      label: label == 'EMG' ? 'Emergency' : 'Military',
      child: Container(
        padding: const EdgeInsets.symmetric(horizontal: 6, vertical: 2),
        decoration: BoxDecoration(color: color, borderRadius: BorderRadius.circular(6)),
        child: Row(mainAxisSize: MainAxisSize.min, children: [
          Icon(icon, size: 12, color: Colors.white),
          const SizedBox(width: 3),
          Text(label,
              style: const TextStyle(color: Colors.white, fontSize: 11, fontWeight: FontWeight.w700)),
        ]),
      ),
    );
  }
}

class _PhotoBox extends StatefulWidget {
  final String reg;
  final String hex;
  final PhotoClient photos;
  const _PhotoBox({required this.reg, required this.hex, required this.photos});

  @override
  State<_PhotoBox> createState() => _PhotoBoxState();
}

class _PhotoBoxState extends State<_PhotoBox> {
  late Future<PhotoRef?> _future;

  @override
  void initState() {
    super.initState();
    _future = widget.photos.lookup(reg: widget.reg, hex: widget.hex);
  }

  @override
  void didUpdateWidget(_PhotoBox old) {
    super.didUpdateWidget(old);
    if (old.reg != widget.reg || old.hex != widget.hex) {
      _future = widget.photos.lookup(reg: widget.reg, hex: widget.hex);
    }
  }

  @override
  Widget build(BuildContext context) {
    final ac = Theme.of(context).extension<AppColors>()!;
    final fill = Theme.of(context).colorScheme.surfaceContainerHighest;
    Widget placeholder() => Container(
          color: fill,
          alignment: Alignment.center,
          child: Icon(Icons.flight, color: ac.muted),
        );

    return ClipRRect(
      borderRadius: BorderRadius.circular(10),
      child: SizedBox(
        width: 112,
        height: 72,
        child: FutureBuilder<PhotoRef?>(
          future: _future,
          builder: (context, snap) {
            final photo = snap.data;
            if (photo == null) return placeholder();
            return Stack(fit: StackFit.expand, children: [
              Image.network(photo.thumbUrl, fit: BoxFit.cover,
                  cacheWidth: 360, // ~2x box width
                  errorBuilder: (_, _, _) => placeholder()),
              Positioned(
                left: 0, right: 0, bottom: 0,
                child: Container(
                  padding: const EdgeInsets.symmetric(horizontal: 4, vertical: 1),
                  color: Colors.black54,
                  child: Text('© ${photo.photographer}',
                      maxLines: 1, overflow: TextOverflow.ellipsis,
                      style: const TextStyle(fontSize: 11, color: Colors.white)),
                ),
              ),
            ]);
          },
        ),
      ),
    );
  }
}
