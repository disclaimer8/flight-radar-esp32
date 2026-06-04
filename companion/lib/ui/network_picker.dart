import 'package:flutter/material.dart';
import '../packet/wifi_scan_packet.dart';

/// Bottom-sheet list of networks the device reported. Pops with the tapped
/// [WifiNetwork] (or null when dismissed).
class NetworkPicker extends StatelessWidget {
  final List<WifiNetwork> networks;
  const NetworkPicker({super.key, required this.networks});

  IconData _signalIcon(int rssi) {
    if (rssi >= -60) return Icons.wifi;
    if (rssi >= -75) return Icons.wifi_2_bar;
    return Icons.wifi_1_bar;
  }

  @override
  Widget build(BuildContext context) {
    return SafeArea(
      child: Column(
        mainAxisSize: MainAxisSize.min,
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          const Padding(
            padding: EdgeInsets.fromLTRB(24, 16, 24, 4),
            child: Text('Networks the device can see',
                style: TextStyle(fontWeight: FontWeight.bold)),
          ),
          Flexible(
            child: ListView.builder(
              shrinkWrap: true,
              itemCount: networks.length,
              itemBuilder: (context, i) {
                final n = networks[i];
                return ListTile(
                  leading: Icon(_signalIcon(n.rssi)),
                  title: Text(n.ssid, maxLines: 1, overflow: TextOverflow.ellipsis),
                  subtitle: Text('${n.rssi} dBm'),
                  trailing: n.secured ? const Icon(Icons.lock_outline) : null,
                  onTap: () => Navigator.pop(context, n),
                );
              },
            ),
          ),
        ],
      ),
    );
  }
}
