import 'package:flutter/material.dart';

/// Semantic colors that aren't part of Material's ColorScheme: the radar accent,
/// emergency/military state colors, and muted text. Read via
/// `Theme.of(context).extension<AppColors>()!` so they adapt to light/dark.
@immutable
class AppColors extends ThemeExtension<AppColors> {
  final Color accent; // radar cyan
  final Color emg; // emergency
  final Color mil; // military
  final Color muted; // secondary text / captions
  final Color cardBorder;

  const AppColors({
    required this.accent,
    required this.emg,
    required this.mil,
    required this.muted,
    required this.cardBorder,
  });

  @override
  AppColors copyWith(
          {Color? accent, Color? emg, Color? mil, Color? muted, Color? cardBorder}) =>
      AppColors(
        accent: accent ?? this.accent,
        emg: emg ?? this.emg,
        mil: mil ?? this.mil,
        muted: muted ?? this.muted,
        cardBorder: cardBorder ?? this.cardBorder,
      );

  @override
  AppColors lerp(ThemeExtension<AppColors>? other, double t) {
    if (other is! AppColors) return this;
    return AppColors(
      accent: Color.lerp(accent, other.accent, t)!,
      emg: Color.lerp(emg, other.emg, t)!,
      mil: Color.lerp(mil, other.mil, t)!,
      muted: Color.lerp(muted, other.muted, t)!,
      cardBorder: Color.lerp(cardBorder, other.cardBorder, t)!,
    );
  }
}

/// Dark-first "aviation instrument / radar HUD" theme: near-black canvas,
/// radar-cyan accent (matches the firmware's cyan BLE indicator), photos as
/// heroes, callsigns set tight/mono-ish so cards read like an instrument.
class AppTheme {
  static const _cyan = Color(0xFF38BDF8);
  static const _cyanDark = Color(0xFF0284C7);
  static const _emg = Color(0xFFFF3B30);
  static const _mil = Color(0xFF18B98A);

  /// Monospace-ish callsign style (no bundled font: system mono fallback).
  static const callsignFamilyFallback = ['ui-monospace', 'Menlo', 'Roboto Mono', 'monospace'];

  static ThemeData dark() {
    const bg = Color(0xFF0B0F14);
    const surface = Color(0xFF141A22);
    const surfaceHi = Color(0xFF1C2530);
    const text = Color(0xFFE6EDF3);
    const muted = Color(0xFF8A97A6);
    final scheme = const ColorScheme.dark(
      primary: _cyan,
      onPrimary: Color(0xFF04141C),
      secondary: _cyan,
      onSecondary: Color(0xFF04141C),
      surface: surface,
      onSurface: text,
      surfaceContainerHighest: surfaceHi,
      error: _emg,
      onError: Colors.white,
    );
    return _base(scheme, bg, muted, const Color(0xFF26303C));
  }

  static ThemeData light() {
    const bg = Color(0xFFF6F8FB);
    const surface = Color(0xFFFFFFFF);
    const text = Color(0xFF0B0F14);
    const muted = Color(0xFF5A6675);
    final scheme = const ColorScheme.light(
      primary: _cyanDark,
      onPrimary: Colors.white,
      secondary: _cyanDark,
      surface: surface,
      onSurface: text,
      surfaceContainerHighest: Color(0xFFEDF1F6),
      error: _emg,
      onError: Colors.white,
    );
    return _base(scheme, bg, muted, const Color(0xFFE2E8F0));
  }

  static ThemeData _base(ColorScheme scheme, Color bg, Color muted, Color border) {
    final accent = scheme.primary;
    return ThemeData(
      useMaterial3: true,
      colorScheme: scheme,
      scaffoldBackgroundColor: bg,
      extensions: [
        AppColors(accent: accent, emg: _emg, mil: _mil, muted: muted, cardBorder: border),
      ],
      appBarTheme: AppBarTheme(
        backgroundColor: bg,
        foregroundColor: scheme.onSurface,
        elevation: 0,
        centerTitle: false,
      ),
      cardTheme: CardThemeData(
        color: scheme.surface,
        elevation: 0,
        shape: RoundedRectangleBorder(
          borderRadius: BorderRadius.circular(16),
          side: BorderSide(color: border),
        ),
        clipBehavior: Clip.antiAlias,
        margin: EdgeInsets.zero,
      ),
      dividerTheme: DividerThemeData(color: border, thickness: 1),
      filledButtonTheme: FilledButtonThemeData(
        style: FilledButton.styleFrom(
          shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(12)),
          padding: const EdgeInsets.symmetric(horizontal: 20, vertical: 14),
        ),
      ),
      inputDecorationTheme: InputDecorationTheme(
        filled: true,
        fillColor: scheme.surfaceContainerHighest,
        border: OutlineInputBorder(
          borderRadius: BorderRadius.circular(12),
          borderSide: BorderSide(color: border),
        ),
        enabledBorder: OutlineInputBorder(
          borderRadius: BorderRadius.circular(12),
          borderSide: BorderSide(color: border),
        ),
      ),
      textTheme: const TextTheme(
        titleMedium: TextStyle(fontSize: 16, fontWeight: FontWeight.w600),
        bodyMedium: TextStyle(fontSize: 14),
        bodySmall: TextStyle(fontSize: 12),
        labelSmall: TextStyle(fontSize: 11),
      ),
    );
  }

  /// Callsign / hero readout style — tight, mono-ish, instrument feel.
  static TextStyle callsign(BuildContext context, {double size = 18}) => TextStyle(
        fontSize: size,
        fontWeight: FontWeight.w700,
        letterSpacing: 0.5,
        fontFamilyFallback: callsignFamilyFallback,
        color: Theme.of(context).colorScheme.onSurface,
      );
}
