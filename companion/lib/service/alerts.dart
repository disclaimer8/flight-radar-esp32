import '../data/aircraft.dart';

/// An aircraft worth alerting on: emergency squawk/status or military.
bool isAlertWorthy(Aircraft a) => a.isEmergency || a.isMilitary;

/// Notification title. Emergency takes precedence when an aircraft is both.
String alertTitle(Aircraft a) => a.isEmergency ? 'Emergency squawk' : 'Military aircraft';

/// Notification body.
String alertBody(Aircraft a) {
  final cs = a.callsign.isEmpty ? '------' : a.callsign;
  if (a.isEmergency) {
    final code = a.squawk?.toString() ?? '';
    return code.isEmpty ? '🚨 $cs' : '🚨 $code: $cs';
  }
  return '$cs ${a.type}'.trim();
}

/// Result of a de-dup pass: aircraft to alert now, and the set of qualifying
/// hexes seen in this cycle (the next cycle's "previously alerted").
typedef AlertPass = ({List<Aircraft> newAlerts, Set<String> alerted});

/// Among [current], pick alert-worthy aircraft (with a non-empty hex) whose hex
/// was not in [previouslyAlerted]. The returned [alerted] set is exactly the
/// qualifying hexes present this cycle — so an aircraft that leaves and returns
/// alerts again, while steady-state presence does not re-alert.
AlertPass computeNewAlerts(List<Aircraft> current, Set<String> previouslyAlerted) {
  final qualifying = current.where((a) => isAlertWorthy(a) && a.hex.isNotEmpty).toList();
  final alerted = qualifying.map((a) => a.hex).toSet();
  final newAlerts = qualifying.where((a) => !previouslyAlerted.contains(a.hex)).toList();
  return (newAlerts: newAlerts, alerted: alerted);
}
