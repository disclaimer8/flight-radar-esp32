import 'dart:io';
import 'dart:typed_data';
import 'dart:ui' as ui;
import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:geocoding/geocoding.dart';
import 'package:latlong2/latlong.dart';
import 'package:path_provider/path_provider.dart';
import 'package:share_plus/share_plus.dart';
import '../data/aircraft.dart';
import '../data/photo_client.dart';
import '../theme/app_theme.dart';

/// Reverse-geocode the observer to a city (best effort), then open a preview of
/// a shareable "catch" card and let the user share it as a PNG. This is the
/// growth loop: a tasteful, watermarked, attribution-baked image of a catch.
Future<void> shareCatch(
    BuildContext context, Aircraft a, PhotoRef? photo, LatLng? observer) async {
  String place = '';
  if (observer != null) {
    try {
      final marks =
          await placemarkFromCoordinates(observer.latitude, observer.longitude);
      if (marks.isNotEmpty) {
        place = marks.first.locality?.isNotEmpty == true
            ? marks.first.locality!
            : (marks.first.administrativeArea ?? '');
      }
    } catch (_) {/* no city; card omits the "over {place}" line */}
  }
  if (!context.mounted) return;
  await showModalBottomSheet(
    context: context,
    isScrollControlled: true,
    builder: (_) => _CatchPreview(aircraft: a, photo: photo, place: place),
  );
}

class _CatchPreview extends StatefulWidget {
  final Aircraft aircraft;
  final PhotoRef? photo;
  final String place;
  const _CatchPreview({required this.aircraft, required this.photo, required this.place});

  @override
  State<_CatchPreview> createState() => _CatchPreviewState();
}

class _CatchPreviewState extends State<_CatchPreview> {
  final _boundaryKey = GlobalKey();
  bool _sharing = false;

  Future<void> _share() async {
    if (_sharing) return;
    setState(() => _sharing = true);
    try {
      final boundary = _boundaryKey.currentContext!.findRenderObject()
          as RenderRepaintBoundary;
      final image = await boundary.toImage(pixelRatio: 3.0);
      final data = await image.toByteData(format: ui.ImageByteFormat.png);
      if (data == null) return;
      final bytes = data.buffer.asUint8List();
      final dir = await getTemporaryDirectory();
      final file = await File('${dir.path}/flight-radar-catch.png')
          .writeAsBytes(bytes, flush: true);
      final cs = widget.aircraft.callsign.isEmpty ? 'aircraft' : widget.aircraft.callsign;
      await SharePlus.instance.share(ShareParams(
        files: [XFile(file.path, mimeType: 'image/png')],
        text: 'Caught $cs with Flight Radar',
      ));
    } catch (_) {/* user cancelled or capture failed */}
    if (mounted) setState(() => _sharing = false);
  }

  @override
  Widget build(BuildContext context) {
    return SafeArea(
      child: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            RepaintBoundary(
              key: _boundaryKey,
              child: CatchCard(
                  aircraft: widget.aircraft, photo: widget.photo, place: widget.place),
            ),
            const SizedBox(height: 16),
            SizedBox(
              width: double.infinity,
              child: FilledButton.icon(
                onPressed: _sharing ? null : _share,
                icon: _sharing
                    ? const SizedBox(
                        width: 18, height: 18,
                        child: CircularProgressIndicator(strokeWidth: 2))
                    : const Icon(Icons.ios_share),
                label: Text(_sharing ? 'Preparing…' : 'Share catch'),
              ),
            ),
          ],
        ),
      ),
    );
  }
}

/// The rendered share artifact. Fixed dark "HUD" composition so it looks the
/// same regardless of app theme; safe to capture offscreen.
class CatchCard extends StatelessWidget {
  final Aircraft aircraft;
  final PhotoRef? photo;
  final String place;
  const CatchCard({super.key, required this.aircraft, required this.photo, this.place = ''});

  @override
  Widget build(BuildContext context) {
    final a = aircraft;
    final cs = a.callsign.isEmpty ? '------' : a.callsign;
    final hasRoute = (a.origin ?? '').isNotEmpty &&
        (a.dest ?? '').isNotEmpty && a.origin != a.dest;
    final now = DateTime.now();
    final date = '${now.day} ${_months[now.month - 1]}';
    final line = [
      if (hasRoute) '${a.origin} → ${a.dest}',
      if (a.distKm != null) '${a.distKm!.round()} km',
      if (a.altFt != null) '${a.altFt} ft',
    ].join(' · ');

    return Container(
      width: 340,
      decoration: BoxDecoration(
        color: const Color(0xFF0B0F14),
        borderRadius: BorderRadius.circular(16),
        border: Border.all(color: const Color(0xFF26303C)),
      ),
      clipBehavior: Clip.antiAlias,
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          AspectRatio(
            aspectRatio: 16 / 9,
            child: photo == null
                ? Container(
                    color: const Color(0xFF1C2530),
                    alignment: Alignment.center,
                    child: const Icon(Icons.flight, size: 56, color: Color(0xFF8A97A6)))
                : Image.network(photo!.thumbUrl, fit: BoxFit.cover, cacheWidth: 1024,
                    errorBuilder: (_, _, _) => Container(
                        color: const Color(0xFF1C2530),
                        alignment: Alignment.center,
                        child: const Icon(Icons.flight, size: 56, color: Color(0xFF8A97A6)))),
          ),
          Padding(
            padding: const EdgeInsets.fromLTRB(16, 12, 16, 14),
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Row(children: [
                  Expanded(
                    child: Text(cs,
                        maxLines: 1, overflow: TextOverflow.ellipsis,
                        style: const TextStyle(
                            color: Color(0xFFE6EDF3), fontSize: 24,
                            fontWeight: FontWeight.w700, letterSpacing: 0.5,
                            fontFamilyFallback: AppTheme.callsignFamilyFallback)),
                  ),
                  if (a.isEmergency) _tag('⚠ EMERGENCY', const Color(0xFFFF3B30)),
                  if (a.isMilitary) _tag('★ MILITARY', const Color(0xFF18B98A)),
                ]),
                const SizedBox(height: 2),
                Text([a.desc.isNotEmpty ? a.desc : a.type, a.registration ?? '']
                        .where((s) => s.isNotEmpty).join(' · '),
                    style: const TextStyle(color: Color(0xFF8A97A6), fontSize: 13)),
                if (line.isNotEmpty)
                  Padding(
                    padding: const EdgeInsets.only(top: 4),
                    child: Text(line,
                        style: const TextStyle(color: Color(0xFFE6EDF3), fontSize: 14)),
                  ),
                const SizedBox(height: 10),
                Text(
                    place.isEmpty ? 'Caught $date' : 'Caught over $place · $date',
                    style: const TextStyle(color: Color(0xFF8A97A6), fontSize: 12)),
                const SizedBox(height: 10),
                const Divider(color: Color(0xFF26303C), height: 1),
                const SizedBox(height: 8),
                Row(children: [
                  const Icon(Icons.flight, size: 14, color: Color(0xFF38BDF8)),
                  const SizedBox(width: 6),
                  const Text('Flight Radar',
                      style: TextStyle(color: Color(0xFF38BDF8), fontSize: 12, fontWeight: FontWeight.w700)),
                  const Spacer(),
                  if (photo != null)
                    Flexible(
                      child: Text('© ${photo!.photographer} / planespotters.net',
                          maxLines: 1, overflow: TextOverflow.ellipsis,
                          textAlign: TextAlign.right,
                          style: const TextStyle(color: Color(0xFF8A97A6), fontSize: 10)),
                    ),
                ]),
              ],
            ),
          ),
        ],
      ),
    );
  }

  Widget _tag(String text, Color color) => Container(
        margin: const EdgeInsets.only(left: 6),
        padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 3),
        decoration: BoxDecoration(color: color, borderRadius: BorderRadius.circular(6)),
        child: Text(text,
            style: const TextStyle(color: Colors.white, fontSize: 10, fontWeight: FontWeight.w700)),
      );
}

const _months = [
  'Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun',
  'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec'
];

/// Re-export for typed Uint8List usage if needed elsewhere.
typedef CatchBytes = Uint8List;
