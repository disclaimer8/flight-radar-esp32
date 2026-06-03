import 'package:flutter/material.dart';
import '../data/aircraft.dart';
import '../data/photo_client.dart';

/// One aircraft row: photo, callsign + badges, type/distance, route, attribution.
/// The photo is looked up lazily from [photos] (foreground only).
class AircraftCard extends StatelessWidget {
  final Aircraft aircraft;
  final PhotoClient photos;
  const AircraftCard({super.key, required this.aircraft, required this.photos});

  bool get _hasRoute =>
      (aircraft.origin ?? '').isNotEmpty &&
      (aircraft.dest ?? '').isNotEmpty &&
      aircraft.origin != aircraft.dest;

  @override
  Widget build(BuildContext context) {
    final cs = aircraft.callsign.isEmpty ? '------' : aircraft.callsign;
    final subtitle = aircraft.desc.isNotEmpty ? aircraft.desc : aircraft.type;
    final dist = aircraft.distKm == null ? '' : ' · ${aircraft.distKm!.round()} km';
    final reg = (aircraft.registration ?? '');

    return Card(
      margin: const EdgeInsets.symmetric(horizontal: 12, vertical: 6),
      child: Padding(
        padding: const EdgeInsets.all(8),
        child: Row(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            _PhotoBox(reg: reg, hex: aircraft.hex, photos: photos),
            const SizedBox(width: 12),
            Expanded(
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Row(children: [
                    Text(cs, style: const TextStyle(fontWeight: FontWeight.bold, fontSize: 16)),
                    const SizedBox(width: 8),
                    if (aircraft.isEmergency) _badge('EMG', Colors.red),
                    if (aircraft.isMilitary) _badge('MIL', Colors.green.shade700),
                  ]),
                  Text('$subtitle$dist'),
                  if (_hasRoute) Text('${aircraft.origin} → ${aircraft.dest}'),
                  if (reg.isNotEmpty) Text(reg),
                ],
              ),
            ),
          ],
        ),
      ),
    );
  }

  Widget _badge(String text, Color color) => Container(
        padding: const EdgeInsets.symmetric(horizontal: 6, vertical: 1),
        decoration: BoxDecoration(color: color, borderRadius: BorderRadius.circular(4)),
        child: Text(text, style: const TextStyle(color: Colors.white, fontSize: 11, fontWeight: FontWeight.bold)),
      );
}

class _PhotoBox extends StatelessWidget {
  final String reg;
  final String hex;
  final PhotoClient photos;
  const _PhotoBox({required this.reg, required this.hex, required this.photos});

  @override
  Widget build(BuildContext context) {
    return SizedBox(
      width: 80,
      height: 60,
      child: FutureBuilder<PhotoRef?>(
        future: photos.lookup(reg: reg, hex: hex),
        builder: (context, snap) {
          final photo = snap.data;
          if (photo == null) {
            return Container(
              color: Colors.black12,
              alignment: Alignment.center,
              child: const Icon(Icons.flight, color: Colors.black38),
            );
          }
          return Column(
            crossAxisAlignment: CrossAxisAlignment.stretch,
            children: [
              Expanded(
                child: Image.network(photo.thumbUrl, fit: BoxFit.cover,
                    errorBuilder: (_, __, ___) => Container(
                        color: Colors.black12,
                        child: const Icon(Icons.flight, color: Colors.black38))),
              ),
              Text('© ${photo.photographer} / planespotters.net',
                  style: const TextStyle(fontSize: 7), overflow: TextOverflow.ellipsis),
            ],
          );
        },
      ),
    );
  }
}
