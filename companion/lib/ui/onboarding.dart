import 'dart:io' show Platform;
import 'package:flutter/material.dart';
import 'package:permission_handler/permission_handler.dart';
import 'package:shared_preferences/shared_preferences.dart';
import '../theme/app_theme.dart';

const _seenKey = 'onboarding_seen_v1';

/// Show the first-run wizard once. Gated on a persisted flag so it never repeats.
Future<void> maybeShowOnboarding(BuildContext context) async {
  final prefs = await SharedPreferences.getInstance();
  if (prefs.getBool(_seenKey) == true) return;
  if (!context.mounted) return;
  await Navigator.of(context).push(MaterialPageRoute(
      fullscreenDialog: true, builder: (_) => const OnboardingScreen()));
  await prefs.setBool(_seenKey, true);
}

class OnboardingScreen extends StatefulWidget {
  const OnboardingScreen({super.key});
  @override
  State<OnboardingScreen> createState() => _OnboardingScreenState();
}

class _OnboardingScreenState extends State<OnboardingScreen> {
  final _page = PageController();
  int _index = 0;

  Future<void> _primeAndRequest() async {
    // Explained on the previous panel, so the OS prompts now have context.
    if (Platform.isIOS) {
      await Permission.bluetooth.request();
      await Permission.locationWhenInUse.request();
    } else {
      await [
        Permission.bluetoothScan,
        Permission.bluetoothConnect,
        Permission.locationWhenInUse,
      ].request();
    }
    _next();
  }

  void _next() {
    if (_index < 2) {
      _page.nextPage(duration: const Duration(milliseconds: 250), curve: Curves.easeOut);
    } else {
      Navigator.of(context).pop();
    }
  }

  @override
  void dispose() {
    _page.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final ac = Theme.of(context).extension<AppColors>()!;
    return Scaffold(
      body: SafeArea(
        child: Column(
          children: [
            Align(
              alignment: Alignment.topRight,
              child: TextButton(
                onPressed: () => Navigator.of(context).pop(),
                child: const Text('Skip'),
              ),
            ),
            Expanded(
              child: PageView(
                controller: _page,
                onPageChanged: (i) => setState(() => _index = i),
                children: const [
                  _Panel(
                    icon: Icons.flight_takeoff,
                    title: 'See what\'s flying overhead',
                    body: 'A live radar of the aircraft around you — callsign, '
                        'type, route, and a photo — right now, no global map to hunt.',
                  ),
                  _Panel(
                    icon: Icons.verified_user_outlined,
                    title: 'Why we need access',
                    body: '📍 Location — to find planes near you.\n'
                        '📡 Bluetooth — to talk to your radar device.\n\n'
                        'No account, no subscription. Your location never leaves your phone.',
                  ),
                  _Panel(
                    icon: Icons.settings_input_antenna,
                    title: 'Connect your device',
                    body: 'Open "Device Wi-Fi setup", scan for networks, and send '
                        'the credentials over Bluetooth. Or just tap Start to watch '
                        'the skies from your phone.',
                  ),
                ],
              ),
            ),
            Row(
              mainAxisAlignment: MainAxisAlignment.center,
              children: List.generate(3, (i) {
                final active = i == _index;
                return AnimatedContainer(
                  duration: const Duration(milliseconds: 200),
                  margin: const EdgeInsets.symmetric(horizontal: 4),
                  width: active ? 20 : 8,
                  height: 8,
                  decoration: BoxDecoration(
                    color: active ? ac.accent : ac.muted,
                    borderRadius: BorderRadius.circular(4),
                  ),
                );
              }),
            ),
            Padding(
              padding: const EdgeInsets.all(24),
              child: SizedBox(
                width: double.infinity,
                child: FilledButton(
                  onPressed: _index == 1 ? _primeAndRequest : _next,
                  child: Text(_index == 0
                      ? 'Next'
                      : _index == 1
                          ? 'Allow access'
                          : 'Get started'),
                ),
              ),
            ),
          ],
        ),
      ),
    );
  }
}

class _Panel extends StatelessWidget {
  final IconData icon;
  final String title;
  final String body;
  const _Panel({required this.icon, required this.title, required this.body});

  @override
  Widget build(BuildContext context) {
    final ac = Theme.of(context).extension<AppColors>()!;
    return Padding(
      padding: const EdgeInsets.symmetric(horizontal: 32),
      child: Column(
        mainAxisAlignment: MainAxisAlignment.center,
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Icon(icon, size: 72, color: ac.accent),
          const SizedBox(height: 28),
          Text(title,
              style: const TextStyle(fontSize: 26, fontWeight: FontWeight.w700, height: 1.1)),
          const SizedBox(height: 14),
          Text(body, style: TextStyle(fontSize: 16, height: 1.4, color: ac.muted)),
        ],
      ),
    );
  }
}
