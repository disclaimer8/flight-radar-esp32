import 'dart:async';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'package:http/http.dart' as http;
import '../packet/photo_ble_packet.dart';
import '../data/photo_client.dart';

enum BleStatus { idle, scanning, connecting, connected, disconnected }

/// Owns the BLE link to the FlightRadar device: scan → connect → write, with
/// reconnect. Designed to run inside the foreground-service isolate.
class BleManager {
  static final Guid serviceUuid = Guid('f1a90001-7e1d-4c2a-9b3f-1a2b3c4d5e6f');
  static final Guid charUuid = Guid('f1a90002-7e1d-4c2a-9b3f-1a2b3c4d5e6f');
  static final Guid photoUuid = Guid('f1a90005-7e1d-4c2a-9b3f-1a2b3c4d5e6f');

  final _statusController = StreamController<BleStatus>.broadcast();
  Stream<BleStatus> get status => _statusController.stream;

  BluetoothDevice? _device;
  BluetoothCharacteristic? _char;
  BluetoothCharacteristic? _photoChar;
  StreamSubscription<List<int>>? _photoSub;
  final PhotoClient _photoClient = PhotoClient();
  final http.Client _http = http.Client();
  StreamSubscription<List<ScanResult>>? _scanSub;
  StreamSubscription<BluetoothConnectionState>? _connSub;
  bool _wantConnected = false;

  BleStatus _status = BleStatus.idle;
  void _set(BleStatus s) {
    _status = s;
    if (!_statusController.isClosed) _statusController.add(s);
  }
  BleStatus get current => _status;

  /// Begin scanning + connecting; keeps trying until [stop] is called.
  Future<void> start() async {
    _wantConnected = true;
    await _scanAndConnect();
  }

  Future<void> _scanAndConnect() async {
    if (!_wantConnected) return;
    _set(BleStatus.scanning);
    await FlutterBluePlus.adapterState.where((s) => s == BluetoothAdapterState.on).first;

    _scanSub?.cancel();
    _scanSub = FlutterBluePlus.onScanResults.listen((results) async {
      if (results.isEmpty) return;
      final r = results.first;
      await FlutterBluePlus.stopScan();
      await _scanSub?.cancel();
      await _connect(r.device);
    });

    await FlutterBluePlus.startScan(
      withServices: [serviceUuid],
      timeout: const Duration(seconds: 15),
    );
  }

  Future<void> _connect(BluetoothDevice device) async {
    _device = device;
    _set(BleStatus.connecting);
    try {
      await device.connect(
        license: License.nonprofit,
        timeout: const Duration(seconds: 15),
      );
      final services = await device.discoverServices();
      _char = null;
      _photoChar = null;
      for (final s in services) {
        if (s.uuid == serviceUuid) {
          for (final c in s.characteristics) {
            if (c.uuid == charUuid) _char = c;
            if (c.uuid == photoUuid) _photoChar = c;
          }
        }
      }

      // stop() may have been called while we were connecting.
      if (!_wantConnected) { await device.disconnect(); return; }

      // Connected but the ingest characteristic is missing → not our device /
      // wrong firmware. Disconnect and retry the scan rather than wedging.
      if (_char == null) {
        await device.disconnect();
        if (_wantConnected) await _scanAndConnect();
        return;
      }

      // Only NOW watch for disconnects. Subscribing after a successful connect
      // means the stream's seeded initial value is `connected`, so we don't get
      // a spurious `disconnected` that would start an overlapping reconnect.
      await _connSub?.cancel();
      _connSub = device.connectionState.listen((state) async {
        if (state == BluetoothConnectionState.disconnected) {
          _set(BleStatus.disconnected);
          _char = null;
          if (_wantConnected) await _scanAndConnect();
        }
      });
      _set(BleStatus.connected);
      await _subscribePhoto();
    } catch (_) {
      _set(BleStatus.disconnected);
      if (_wantConnected) await _scanAndConnect();
    }
  }

  /// Write one packet. Returns true if a connected characteristic accepted it.
  Future<bool> sendPacket(List<int> bytes) async {
    final c = _char;
    if (c == null) return false;
    try {
      await c.write(bytes, withoutResponse: false); // write-with-response
      return true;
    } catch (_) {
      return false;
    }
  }

  Future<void> _subscribePhoto() async {
    final pc = _photoChar;
    if (pc == null) return; // older firmware without the photo characteristic
    try {
      await _photoSub?.cancel();
      await pc.setNotifyValue(true);
      _photoSub = pc.onValueReceived.listen((bytes) {
        final req = parsePhotoReq(bytes);
        if (req != null) _servePhoto(req); // fire-and-forget
      });
    } catch (_) {/* photo feature stays off this session */}
  }

  // Fetch the requested aircraft's photo and stream it back. The device key is a
  // registration or a hex; try it as both (reg first, then hex).
  Future<void> _servePhoto(PhotoReq req) async {
    final pc = _photoChar;
    if (pc == null) return;
    try {
      final ref = await _photoClient.lookup(reg: req.key, hex: req.key);
      if (ref == null) {
        await pc.write(buildPhotoHeader(req.reqId, 0, ''), withoutResponse: false);
        return;
      }
      final url = buildProxiedPhotoUrl(ref.thumbUrl, quality: 55);
      final resp =
          await _http.get(Uri.parse(url)).timeout(const Duration(seconds: 10));
      if (resp.statusCode != 200 || resp.bodyBytes.isEmpty) {
        await pc.write(buildPhotoHeader(req.reqId, 0, ''), withoutResponse: false);
        return;
      }
      final jpeg = resp.bodyBytes;
      await pc.write(
          buildPhotoHeader(req.reqId, jpeg.length, ref.photographer),
          withoutResponse: false);
      // Conservative payload from the negotiated MTU (ATT 3 + our 6-byte header).
      final device = _device;
      final mtu = device?.mtuNow ?? 23;
      final payload = (mtu - 9).clamp(20, 500);
      for (final frame in chunkJpeg(req.reqId, jpeg, payload)) {
        await pc.write(frame, withoutResponse: false); // ordered + flow-controlled
      }
    } catch (_) {
      try {
        await pc.write(buildPhotoHeader(req.reqId, 0, ''), withoutResponse: false);
      } catch (_) {}
    }
  }

  Future<void> stop() async {
    _wantConnected = false;
    await _scanSub?.cancel();
    await _connSub?.cancel();
    await _photoSub?.cancel();
    _photoChar = null;
    try { await FlutterBluePlus.stopScan(); } catch (_) {}
    try { await _device?.disconnect(); } catch (_) {}
    _char = null;
    _set(BleStatus.idle);
  }

  Future<void> dispose() async {
    await stop();
    await _statusController.close();
  }
}
