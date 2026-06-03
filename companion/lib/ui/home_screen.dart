import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter_foreground_task/flutter_foreground_task.dart';
import 'package:permission_handler/permission_handler.dart';
import '../service/gateway_controller.dart';

class HomeScreen extends StatefulWidget {
  const HomeScreen({super.key});
  @override
  State<HomeScreen> createState() => _HomeScreenState();
}

class _HomeScreenState extends State<HomeScreen> {
  final _controller = GatewayController();
  GatewayStatus _status = const GatewayStatus();
  bool _running = false;
  StreamSubscription<GatewayStatus>? _sub;

  @override
  void initState() {
    super.initState();
    _controller.init();
    _sub = _controller.status.listen((s) {
      if (mounted) setState(() => _status = s);
    });
  }

  /// Request every runtime permission the foreground service needs BEFORE start.
  /// The location+connectedDevice FGS types require Bluetooth AND location to be
  /// granted at startForeground time, else Android throws a SecurityException and
  /// the service is killed. Returns true only if the required ones are granted.
  Future<bool> _requestPermissions() async {
    // Notification (Android 13+): for the persistent FGS notification.
    final notif = await FlutterForegroundTask.checkNotificationPermission();
    if (notif != NotificationPermission.granted) {
      await FlutterForegroundTask.requestNotificationPermission();
    }
    // Bluetooth (Android 12+) + foreground location are REQUIRED for the FGS.
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
      body: Padding(
        padding: const EdgeInsets.all(24),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            _row('Device', _status.ble),
            _row('GPS', _status.fix),
            _row('Last packet', '${_status.count} aircraft'),
            const Spacer(),
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
