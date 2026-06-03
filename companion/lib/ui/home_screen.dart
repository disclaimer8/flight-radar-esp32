import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter_foreground_task/flutter_foreground_task.dart';
import 'package:geolocator/geolocator.dart';
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

  Future<void> _requestPermissions() async {
    final perm = await FlutterForegroundTask.checkNotificationPermission();
    if (perm != NotificationPermission.granted) {
      await FlutterForegroundTask.requestNotificationPermission();
    }
    var p = await Geolocator.checkPermission();
    if (p == LocationPermission.denied) p = await Geolocator.requestPermission();
  }

  Future<void> _toggle() async {
    if (_running) {
      await _controller.stop();
      setState(() => _running = false);
    } else {
      await _requestPermissions();
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
