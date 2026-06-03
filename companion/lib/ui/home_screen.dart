import 'dart:async';
import 'dart:io' show Platform;

import 'package:flutter/material.dart';
import 'package:flutter_foreground_task/flutter_foreground_task.dart';
import 'package:permission_handler/permission_handler.dart';
import '../data/photo_client.dart';
import '../service/gateway_controller.dart';
import '../service/gateway_engine.dart' show GatewayStatus;
import 'aircraft_card.dart';

class HomeScreen extends StatefulWidget {
  const HomeScreen({super.key});
  @override
  State<HomeScreen> createState() => _HomeScreenState();
}

class _HomeScreenState extends State<HomeScreen> {
  final _controller = GatewayController();
  GatewayStatus _status = const GatewayStatus();
  bool _running = false;
  final _photos = PhotoClient();
  StreamSubscription<GatewayStatus>? _sub;

  @override
  void initState() {
    super.initState();
    _controller.init();
    _sub = _controller.status.listen((s) {
      if (mounted) setState(() => _status = s);
    });
  }

  /// Request every runtime permission the gateway needs BEFORE start. On Android
  /// the location|connectedDevice FGS types require Bluetooth AND location granted
  /// at startForeground time. On iOS, background feeding needs "Always" location
  /// (requested as When-in-Use first, then escalated). Returns true if the
  /// required permissions are granted.
  Future<bool> _requestPermissions() async {
    if (Platform.isIOS) {
      final bt = await Permission.bluetooth.request();
      final whenInUse = await Permission.locationWhenInUse.request();
      if (whenInUse.isGranted) {
        // Background needs "Always"; iOS shows this as a follow-up upgrade prompt.
        final always = await Permission.locationAlways.request();
        if (!always.isGranted && mounted) {
          ScaffoldMessenger.of(context).showSnackBar(const SnackBar(
            content: Text('Enable "Always" location to keep feeding in the background'),
          ));
        }
      }
      return bt.isGranted && whenInUse.isGranted;
    }
    // Android.
    final notif = await FlutterForegroundTask.checkNotificationPermission();
    if (notif != NotificationPermission.granted) {
      await FlutterForegroundTask.requestNotificationPermission();
    }
    final statuses = await [
      Permission.bluetoothScan,
      Permission.bluetoothConnect,
      Permission.locationWhenInUse,
    ].request();
    return statuses.values.every((s) => s.isGranted);
  }

  Future<void> _toggle() async {
    if (_running) {
      await _controller.stop();
      setState(() => _running = false);
    } else {
      final granted = await _requestPermissions();
      if (!granted) {
        if (mounted) {
          ScaffoldMessenger.of(context).showSnackBar(const SnackBar(
            content: Text('Bluetooth and location permissions are required'),
          ));
        }
        return;
      }
      final ok = await _controller.start();
      setState(() => _running = ok);
    }
  }

  @override
  void dispose() {
    _sub?.cancel();
    _controller.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('Flight Radar Companion')),
      body: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          Padding(
            padding: const EdgeInsets.fromLTRB(24, 16, 24, 8),
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                _row('Device', _status.ble),
                _row('GPS', _status.fix),
                _row('Last packet', '${_status.count} aircraft'),
                const SizedBox(height: 12),
                SizedBox(
                  width: double.infinity,
                  child: FilledButton(
                    onPressed: _toggle,
                    child: Text(_running ? 'Stop' : 'Start feeding device'),
                  ),
                ),
              ],
            ),
          ),
          const Divider(height: 1),
          Expanded(
            child: _status.aircraft.isEmpty
                ? const Center(
                    child: Text('Start feeding to see nearby aircraft',
                        style: TextStyle(color: Colors.black54)))
                : ListView.builder(
                    itemCount: _status.aircraft.length,
                    itemBuilder: (context, i) => AircraftCard(
                        key: ValueKey(_status.aircraft[i].hex),
                        aircraft: _status.aircraft[i],
                        photos: _photos),
                  ),
          ),
        ],
      ),
    );
  }

  Widget _row(String label, String value) => Padding(
        padding: const EdgeInsets.symmetric(vertical: 8),
        child: Row(
          mainAxisAlignment: MainAxisAlignment.spaceBetween,
          children: [Text(label), Text(value, style: const TextStyle(fontWeight: FontWeight.bold))],
        ),
      );
}
