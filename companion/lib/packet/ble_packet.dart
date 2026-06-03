import 'dart:typed_data';
import '../data/aircraft.dart';

// Wire protocol — must match ../src/ble_core.h (little-endian).
const int bleMagic0 = 0x46; // 'F'
const int bleMagic1 = 0x52; // 'R'
const int bleVersion = 2;
const int bleMaxAircraft = 15;
const int bleHeaderSize = 12;
const int bleRecordSize = 32;
const int bleMaxPacket = bleHeaderSize + bleMaxAircraft * bleRecordSize; // 492

const int bleFlagGround = 0x01;
const int bleFlagAltValid = 0x02;
const int bleFlagGsValid = 0x04;
const int bleFlagTrackValid = 0x08;
const int bleFlagSquawkValid = 0x10;

/// Encode one packet: 12-byte header + up to 15 × 32-byte records.
/// Records beyond 15 are dropped (the caller passes them nearest-first).
Uint8List encodePacket(double centerLat, double centerLon, List<Aircraft> aircraft) {
  final n = aircraft.length > bleMaxAircraft ? bleMaxAircraft : aircraft.length;
  final out = Uint8List(bleHeaderSize + n * bleRecordSize);
  final bd = ByteData.sublistView(out);

  out[0] = bleMagic0;
  out[1] = bleMagic1;
  out[2] = bleVersion;
  out[3] = n;
  bd.setFloat32(4, centerLat, Endian.little);
  bd.setFloat32(8, centerLon, Endian.little);

  for (var i = 0; i < n; i++) {
    final a = aircraft[i];
    final base = bleHeaderSize + i * bleRecordSize;
    _writeField(out, base, 8, a.callsign);
    _writeField(out, base + 8, 4, a.type);
    bd.setFloat32(base + 12, a.lat, Endian.little);
    bd.setFloat32(base + 16, a.lon, Endian.little);
    bd.setInt32(base + 20, a.altFt ?? 0, Endian.little);
    bd.setInt16(base + 24, a.gsKt ?? 0, Endian.little);
    var flags = 0;
    if (a.onGround) flags |= bleFlagGround;
    if (a.altFt != null) flags |= bleFlagAltValid;
    if (a.gsKt != null) flags |= bleFlagGsValid;
    if (a.track != null) flags |= bleFlagTrackValid;
    if (a.squawk != null) flags |= bleFlagSquawkValid;
    out[base + 26] = flags;
    out[base + 27] = 0; // pad
    bd.setInt16(base + 28, a.track?.round() ?? 0, Endian.little);
    bd.setUint16(base + 30, a.squawk ?? 0, Endian.little);
  }
  return out;
}

/// Write an ASCII field of fixed width: truncate if longer, space-pad if shorter.
/// Non-ASCII code units are masked to 7 bits (callsigns/types are ICAO [A-Z0-9],
/// so this never triggers; ble_send.py instead drops non-ASCII — harmless skew).
void _writeField(Uint8List out, int offset, int width, String s) {
  for (var i = 0; i < width; i++) {
    out[offset + i] = i < s.length ? (s.codeUnitAt(i) & 0x7f) : 0x20; // space
  }
}
