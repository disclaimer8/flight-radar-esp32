import 'dart:convert';
import 'dart:typed_data';

// Mirror of src/photo_ble_core.h (byte-exact). Characteristic f1a90005.
const int photoBleMagic = 0x50; // 'P'
const int photoBleTReq = 0x52; // 'R'
const int photoBleTHeader = 0x48; // 'H'
const int photoBleTData = 0x44; // 'D'
const int photoBleVersion = 1;
const int photoBleMaxKey = 11;
const int photoBleMaxCred = 47;

/// A parsed photo request (device -> phone).
class PhotoReq {
  final int reqId;
  final String key;
  const PhotoReq(this.reqId, this.key);
}

/// Parse a PR notify. Returns null on malformed bytes.
PhotoReq? parsePhotoReq(List<int> bytes) {
  if (bytes.length < 5) return null;
  if (bytes[0] != photoBleMagic || bytes[1] != photoBleTReq) return null;
  if (bytes[2] != photoBleVersion) return null;
  final reqId = bytes[3];
  final keyLen = bytes[4];
  if (keyLen == 0 || keyLen > photoBleMaxKey) return null;
  if (bytes.length < 5 + keyLen) return null;
  final key = utf8.decode(bytes.sublist(5, 5 + keyLen), allowMalformed: true);
  return PhotoReq(reqId, key);
}

/// Build a PH header frame. `credit` is truncated to photoBleMaxCred bytes.
Uint8List buildPhotoHeader(int reqId, int totalLen, String credit) {
  final cred = utf8.encode(credit);
  final c = cred.length > photoBleMaxCred ? cred.sublist(0, photoBleMaxCred) : cred;
  final b = BytesBuilder();
  b.add([photoBleMagic, photoBleTHeader, photoBleVersion, reqId & 0xFF]);
  final lenBytes = Uint8List(4)
    ..buffer.asByteData().setUint32(0, totalLen, Endian.little);
  b.add(lenBytes);
  b.add([c.length]);
  b.add(c);
  return b.toBytes();
}

/// Build a PD data frame carrying [chunk] with sequence [seq].
Uint8List buildPhotoChunk(int reqId, int seq, List<int> chunk) {
  final b = BytesBuilder();
  b.add([photoBleMagic, photoBleTData, photoBleVersion, reqId & 0xFF]);
  final seqBytes = Uint8List(2)
    ..buffer.asByteData().setUint16(0, seq, Endian.little);
  b.add(seqBytes);
  b.add(chunk);
  return b.toBytes();
}

/// Split [jpeg] into PD frames whose payload is at most [maxPayload] bytes.
List<Uint8List> chunkJpeg(int reqId, List<int> jpeg, int maxPayload) {
  final out = <Uint8List>[];
  var seq = 0;
  for (var off = 0; off < jpeg.length; off += maxPayload) {
    final end = (off + maxPayload < jpeg.length) ? off + maxPayload : jpeg.length;
    out.add(buildPhotoChunk(reqId, seq, jpeg.sublist(off, end)));
    seq++;
  }
  return out;
}

/// Mirror of photo_core.h buildProxiedPhotoUrl, plus a quality knob for BLE.
/// wsrv.nl re-encodes the (progressive) planespotters thumb into a baseline,
/// cover-cropped 240x240 JPEG; lower [quality] shrinks the BLE transfer.
String buildProxiedPhotoUrl(String src, {int quality = 55}) {
  var bare = src;
  if (bare.startsWith('https://')) {
    bare = bare.substring(8);
  } else if (bare.startsWith('http://')) {
    bare = bare.substring(7);
  }
  return 'https://wsrv.nl/?url=$bare&w=240&h=240&fit=cover&output=jpg&q=$quality';
}
