import 'dart:async';
import 'dart:io' show Platform;

import 'package:flutter/material.dart';
import 'package:flutter_foreground_task/flutter_foreground_task.dart';
import 'package:permission_handler/permission_handler.dart';
import '../ble/wifi_provisioner.dart';
import '../ble/wifi_scanner.dart';
import '../data/photo_client.dart';
import '../packet/wifi_scan_packet.dart';
import '../service/gateway_controller.dart';
import '../service/gateway_engine.dart' show GatewayStatus;
import 'aircraft_card.dart';
import 'network_picker.dart';

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
  final _ssidCtrl = TextEditingController();
  final _passCtrl = TextEditingController();
  final _provisioner = WifiProvisioner();
  String _provStatus = '';
  bool _provisioning = false;
  bool _scanning = false;
  final _passFocus = FocusNode();
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
  /// BLE-only permissions for the on-demand provisioning scan/connect (the
  /// feeder's full permission set isn't needed just to send Wi-Fi creds).
  Future<bool> _requestBlePermissions() async {
    if (Platform.isIOS) {
      final bt = await Permission.bluetooth.request();
      return bt.isGranted;
    }
    final statuses = await [
      Permission.bluetoothScan,
      Permission.bluetoothConnect,
    ].request();
    return statuses.values.every((s) => s.isGranted);
  }

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

  Future<void> _sendWifi() async {
    if (_ssidCtrl.text.isEmpty || _provisioning) return;
    if (!await _requestBlePermissions()) {
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(const SnackBar(
          content: Text('Bluetooth permission is required to configure the device'),
        ));
      }
      return;
    }
    setState(() { _provisioning = true; _provStatus = 'Connecting to device…'; });
    final sub = _provisioner.states.listen((s) {
      if (!mounted) return;
      setState(() {
        switch (s.phase) {
          case ProvPhase.connecting: _provStatus = 'Connecting to device…'; break;
          case ProvPhase.sending:    _provStatus = 'Sending credentials…'; break;
          case ProvPhase.applying:   _provStatus = 'Device joining Wi-Fi…'; break;
          case ProvPhase.connected:  _provStatus = 'Connected: ${s.detail}'; break;
          case ProvPhase.failed:     _provStatus = 'Failed: ${s.detail}'; break;
          case ProvPhase.idle:       break;
        }
      });
    });
    try {
      await _provisioner.provision(_ssidCtrl.text, _passCtrl.text);
    } finally {
      await sub.cancel();
      if (mounted) setState(() => _provisioning = false);
    }
  }

  Future<void> _scanWifi() async {
    if (_running || _provisioning || _scanning) return;
    if (!await _requestBlePermissions()) {
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(const SnackBar(
          content: Text('Bluetooth permission is required to scan'),
        ));
      }
      return;
    }
    setState(() { _scanning = true; _provStatus = 'Scanning networks…'; });
    try {
      final nets = await WifiScanner().scan();
      if (!mounted) return;
      setState(() => _provStatus = nets.isEmpty ? 'No networks found' : '');
      if (nets.isEmpty) return;
      final picked = await showModalBottomSheet<WifiNetwork>(
        context: context,
        builder: (_) => NetworkPicker(networks: nets),
      );
      if (picked != null && mounted) {
        _ssidCtrl.text = picked.ssid;
        _passFocus.requestFocus();
      }
    } on WifiScanException catch (e) {
      if (mounted) setState(() => _provStatus = 'Scan failed: $e');
    } finally {
      if (mounted) setState(() => _scanning = false);
    }
  }

  @override
  void dispose() {
    _ssidCtrl.dispose();
    _passCtrl.dispose();
    _passFocus.dispose();
    _provisioner.dispose();
    _sub?.cancel();
    _controller.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    // The whole screen is one scroll view: in landscape the fixed status +
    // Wi-Fi sections alone are taller than the viewport, so a plain Column
    // overflows.
    return Scaffold(
      appBar: AppBar(title: const Text('Flight Radar Companion')),
      body: CustomScrollView(
        slivers: [
          SliverToBoxAdapter(
            child: Padding(
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
          ),
          const SliverToBoxAdapter(child: Divider(height: 1)),
          SliverToBoxAdapter(
            child: Padding(
              padding: const EdgeInsets.fromLTRB(24, 12, 24, 8),
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  const Text('Configure device Wi-Fi',
                      style: TextStyle(fontWeight: FontWeight.bold)),
                  Row(
                    crossAxisAlignment: CrossAxisAlignment.end,
                    children: [
                      Expanded(
                        child: TextField(
                          controller: _ssidCtrl,
                          decoration: const InputDecoration(labelText: 'SSID'),
                        ),
                      ),
                      IconButton(
                        tooltip: 'Scan networks via device',
                        onPressed:
                            (_running || _provisioning || _scanning) ? null : _scanWifi,
                        icon: _scanning
                            ? const SizedBox(
                                width: 20, height: 20,
                                child: CircularProgressIndicator(strokeWidth: 2))
                            : const Icon(Icons.wifi_find),
                      ),
                    ],
                  ),
                  TextField(
                    controller: _passCtrl,
                    focusNode: _passFocus,
                    obscureText: true,
                    decoration: const InputDecoration(labelText: 'Password'),
                  ),
                  const SizedBox(height: 8),
                  Row(
                    children: [
                      FilledButton(
                        onPressed: (_running || _provisioning) ? null : _sendWifi,
                        child: const Text('Send to device'),
                      ),
                      const SizedBox(width: 12),
                      Expanded(
                        child: Text(
                          _running ? 'Stop feeding to configure device Wi-Fi' : _provStatus,
                          style: const TextStyle(color: Colors.black54),
                          maxLines: 2,
                          overflow: TextOverflow.ellipsis,
                        ),
                      ),
                    ],
                  ),
                ],
              ),
            ),
          ),
          const SliverToBoxAdapter(child: Divider(height: 1)),
          if (_status.aircraft.isEmpty)
            const SliverFillRemaining(
              hasScrollBody: false,
              child: Center(
                  child: Text('Start feeding to see nearby aircraft',
                      style: TextStyle(color: Colors.black54))),
            )
          else
            SliverList.builder(
              itemCount: _status.aircraft.length,
              itemBuilder: (context, i) => AircraftCard(
                  key: ValueKey(_status.aircraft[i].hex),
                  aircraft: _status.aircraft[i],
                  photos: _photos),
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
