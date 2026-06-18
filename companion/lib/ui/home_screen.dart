import 'dart:async';
import 'dart:io' show Platform;

import 'package:flutter/material.dart';
import 'package:flutter_foreground_task/flutter_foreground_task.dart';
import 'package:permission_handler/permission_handler.dart';
import '../ble/wifi_provisioner.dart';
import '../ble/wifi_scanner.dart';
import '../data/aircraft.dart';
import '../data/photo_client.dart';
import '../packet/wifi_scan_packet.dart';
import '../service/gateway_controller.dart';
import '../service/gateway_engine.dart' show GatewayStatus;
import '../theme/app_theme.dart';
import 'aircraft_card.dart';
import 'aircraft_detail_sheet.dart';
import 'network_picker.dart';
import 'onboarding.dart';

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
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (mounted) maybeShowOnboarding(context);
    });
  }

  /// BLE-only permissions for the on-demand provisioning scan/connect.
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

  /// Every runtime permission the gateway needs BEFORE start. Android
  /// location|connectedDevice FGS types require Bluetooth + location granted at
  /// startForeground; iOS background feeding needs "Always" location.
  Future<bool> _requestPermissions() async {
    if (Platform.isIOS) {
      final bt = await Permission.bluetooth.request();
      final whenInUse = await Permission.locationWhenInUse.request();
      if (whenInUse.isGranted) {
        final always = await Permission.locationAlways.request();
        if (!always.isGranted && mounted) {
          ScaffoldMessenger.of(context).showSnackBar(const SnackBar(
            content: Text('Enable "Always" location to keep feeding in the background'),
          ));
        }
      }
      return bt.isGranted && whenInUse.isGranted;
    }
    // Android: request notification permission here (the only place) so the
    // foreground-service isolate doesn't double-prompt.
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
    if (_provisioning || _scanning) return;
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
    if (_ssidCtrl.text.isEmpty || _running || _provisioning || _scanning) return;
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

  /// Parse the engine's "lat, lon" fix string into observer coords (for bearing).
  (double?, double?) _observer() {
    final parts = _status.fix.split(',');
    if (parts.length == 2) {
      final la = double.tryParse(parts[0].trim());
      final lo = double.tryParse(parts[1].trim());
      if (la != null && lo != null) return (la, lo);
    }
    return (null, null);
  }

  /// EMG/MIL pinned to top, then nearest-first.
  List<Aircraft> _sorted() {
    final list = [..._status.aircraft];
    list.sort((a, b) {
      final pa = (a.isEmergency || a.isMilitary) ? 0 : 1;
      final pb = (b.isEmergency || b.isMilitary) ? 0 : 1;
      if (pa != pb) return pa - pb;
      return (a.distKm ?? 1e9).compareTo(b.distKm ?? 1e9);
    });
    return list;
  }

  @override
  Widget build(BuildContext context) {
    final (obsLat, obsLon) = _observer();
    final aircraft = _sorted();
    return Scaffold(
      appBar: AppBar(
        title: const Text('Flight Radar'),
        actions: [
          _StatusPill(running: _running, bleState: _status.ble),
          const SizedBox(width: 12),
        ],
      ),
      floatingActionButton: FloatingActionButton.extended(
        onPressed: (_provisioning || _scanning) ? null : _toggle,
        icon: Icon(_running ? Icons.stop_rounded : Icons.play_arrow_rounded),
        label: Text(_running ? 'Stop' : 'Start'),
      ),
      body: CustomScrollView(
        slivers: [
          SliverToBoxAdapter(child: _setupSection(context)),
          if (aircraft.isEmpty)
            SliverFillRemaining(hasScrollBody: false, child: _emptyState(context))
          else
            SliverPadding(
              padding: const EdgeInsets.only(top: 4, bottom: 96),
              sliver: SliverList.builder(
                itemCount: aircraft.length,
                itemBuilder: (context, i) {
                  final a = aircraft[i];
                  return AircraftCard(
                    key: ValueKey(a.hex),
                    aircraft: a,
                    photos: _photos,
                    observerLat: obsLat,
                    observerLon: obsLon,
                    onTap: () =>
                        showAircraftDetail(context, a, _photos, _controller.status),
                  );
                },
              ),
            ),
        ],
      ),
    );
  }

  Widget _emptyState(BuildContext context) {
    final ac = Theme.of(context).extension<AppColors>()!;
    final (title, sub) = _running
        ? ('Quiet skies right now', "We'll buzz you when something flies over ✈")
        : ('Watch the skies above you', "Tap Start to see what's flying overhead.");
    return Padding(
      padding: const EdgeInsets.all(32),
      child: Column(
        mainAxisAlignment: MainAxisAlignment.center,
        children: [
          Icon(_running ? Icons.radar : Icons.flight_takeoff, size: 56, color: ac.accent),
          const SizedBox(height: 16),
          Text(title, style: const TextStyle(fontSize: 18, fontWeight: FontWeight.w600)),
          const SizedBox(height: 6),
          Text(sub, textAlign: TextAlign.center, style: TextStyle(color: ac.muted)),
        ],
      ),
    );
  }

  Widget _setupSection(BuildContext context) {
    final ac = Theme.of(context).extension<AppColors>()!;
    return Theme(
      data: Theme.of(context).copyWith(dividerColor: Colors.transparent),
      child: ExpansionTile(
        leading: const Icon(Icons.settings_input_antenna),
        title: const Text('Device Wi-Fi setup'),
        subtitle: Text('Connect your radar to Wi-Fi over Bluetooth',
            style: TextStyle(fontSize: 12, color: ac.muted)),
        childrenPadding: const EdgeInsets.fromLTRB(16, 0, 16, 16),
        children: [
          Row(
            crossAxisAlignment: CrossAxisAlignment.end,
            children: [
              Expanded(
                child: TextField(
                  controller: _ssidCtrl,
                  decoration: const InputDecoration(labelText: 'Wi-Fi name (SSID)'),
                ),
              ),
              const SizedBox(width: 8),
              IconButton.filledTonal(
                tooltip: 'Scan networks via device',
                onPressed: (_running || _provisioning || _scanning) ? null : _scanWifi,
                icon: _scanning
                    ? const SizedBox(
                        width: 20, height: 20,
                        child: CircularProgressIndicator(strokeWidth: 2))
                    : const Icon(Icons.wifi_find),
              ),
            ],
          ),
          const SizedBox(height: 8),
          TextField(
            controller: _passCtrl,
            focusNode: _passFocus,
            obscureText: true,
            decoration: const InputDecoration(labelText: 'Password'),
          ),
          const SizedBox(height: 12),
          Row(
            children: [
              FilledButton(
                onPressed:
                    (_running || _provisioning || _scanning) ? null : _sendWifi,
                child: const Text('Send to device'),
              ),
              const SizedBox(width: 12),
              Expanded(
                child: Text(
                  _running ? 'Stop feeding to configure Wi-Fi' : _provStatus,
                  style: TextStyle(color: ac.muted),
                  maxLines: 2,
                  overflow: TextOverflow.ellipsis,
                ),
              ),
            ],
          ),
        ],
      ),
    );
  }
}

/// Compact connection state: colored dot + friendly word (no raw enum text).
class _StatusPill extends StatelessWidget {
  final bool running;
  final String bleState;
  const _StatusPill({required this.running, required this.bleState});

  @override
  Widget build(BuildContext context) {
    final ac = Theme.of(context).extension<AppColors>()!;
    final scheme = Theme.of(context).colorScheme;
    late final String label;
    late final Color color;
    if (!running) {
      label = 'Off';
      color = ac.muted;
    } else if (bleState == 'connected') {
      label = 'Linked';
      color = ac.accent;
    } else if (bleState == 'scanning' || bleState == 'connecting') {
      label = 'Searching';
      color = Colors.amber;
    } else {
      label = 'No device';
      color = ac.muted;
    }
    return Semantics(
      label: 'Connection: $label',
      child: Container(
        padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 6),
        decoration: BoxDecoration(
          color: scheme.surfaceContainerHighest,
          borderRadius: BorderRadius.circular(20),
        ),
        child: Row(mainAxisSize: MainAxisSize.min, children: [
          Container(width: 8, height: 8, decoration: BoxDecoration(color: color, shape: BoxShape.circle)),
          const SizedBox(width: 6),
          Text(label, style: const TextStyle(fontSize: 12, fontWeight: FontWeight.w600)),
        ]),
      ),
    );
  }
}
