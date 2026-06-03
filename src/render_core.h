#pragma once
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
#include "flight_core.h"   // Aircraft, ftToM, ktToKmh

// Initial great-circle bearing observer->target, degrees, north=0, clockwise, [0,360).
inline double bearingDeg(double lat1, double lon1, double lat2, double lon2) {
    const double toRad = M_PI / 180.0;
    double dLon = (lon2 - lon1) * toRad;
    double y = std::sin(dLon) * std::cos(lat2 * toRad);
    double x = std::cos(lat1 * toRad) * std::sin(lat2 * toRad) -
               std::sin(lat1 * toRad) * std::cos(lat2 * toRad) * std::cos(dLon);
    double b = std::atan2(y, x) * 180.0 / M_PI;
    return std::fmod(b + 360.0, 360.0);
}

struct ScreenPoint { int x; int y; };

// Map (bearing, distance) to screen pixels. North up, screen Y grows downward.
// Distance is clamped to [0, rangeKm]; radius scales linearly to maxRadiusPx.
inline ScreenPoint polarToXY(double bearing, double distKm, double rangeKm,
                             int cx, int cy, int maxRadiusPx) {
    double d = distKm;
    if (d < 0) d = 0;
    if (d > rangeKm) d = rangeKm;
    double r = (rangeKm > 0) ? (d / rangeKm) * maxRadiusPx : 0.0;
    double th = bearing * M_PI / 180.0;
    ScreenPoint p;
    p.x = (int)std::lround(cx + r * std::sin(th));
    p.y = (int)std::lround(cy - r * std::cos(th));
    return p;
}

// Short, non-padded display strings for the detail card.
inline std::string fmtDist(double distKm) {
    long km = std::lround(distKm);
    if (km < 0) km = 0;
    // Intentional sanity cap from the LCD era: distances above ~999 km aren't meaningful here.
    if (km > 999) km = 999;
    char b[16];
    std::snprintf(b, sizeof(b), "%ld km", km);
    return b;
}

inline std::string fmtAlt(const Aircraft& ac) {
    if (ac.onGround) return "GND";
    if (std::isnan(ac.altFt)) return "---";
    long m = std::lround(ftToM(ac.altFt));
    if (m > 99999) m = 99999;
    if (m < -9999) m = -9999;
    char b[12];
    std::snprintf(b, sizeof(b), "%ldm", m);
    return b;
}

inline std::string fmtSpeed(const Aircraft& ac) {
    if (std::isnan(ac.gsKt)) return "---";
    long s = std::lround(ktToKmh(ac.gsKt));
    if (s < 0) s = 0;
    if (s > 9999) s = 9999;
    char b[12];
    std::snprintf(b, sizeof(b), "%ld", s);
    return b;
}

// Endpoint of a fixed-length line from `from` along `headingDeg` (north-up:
// 0 = up/north, 90 = right/east). Used to draw aircraft heading vectors.
inline ScreenPoint vectorEnd(ScreenPoint from, double headingDeg, double length) {
    double r = headingDeg * M_PI / 180.0;
    return ScreenPoint{
        (int)std::lround(from.x + length * std::sin(r)),
        (int)std::lround(from.y - length * std::cos(r)),
    };
}

// Altitude band index for blip color: 0 ground/unknown, 1 <3k, 2 3-10k,
// 3 10-25k, 4 25-40k, 5 >40k (feet).
inline int altBand(double altFt, bool onGround) {
    if (onGround || std::isnan(altFt)) return 0;
    if (altFt < 3000)  return 1;
    if (altFt < 10000) return 2;
    if (altFt < 25000) return 3;
    if (altFt < 40000) return 4;
    return 5;
}

// Emergency transponder codes: 7500 hijack, 7600 radio fail, 7700 general.
inline bool isEmergencySquawk(int code) {
    return code == 7500 || code == 7600 || code == 7700;
}

// 8-point compass rose label for a bearing in degrees.
inline const char* compassPoint(double bearing) {
    static const char* pts[8] = {"N","NE","E","SE","S","SW","W","NW"};
    double b = std::fmod(bearing + 360.0, 360.0);
    int idx = ((int)std::lround(b / 45.0)) % 8;
    return pts[idx];
}

// Split a hexdb.io route string ("EGLL-KJFK", possibly multi-leg) into
// (origin, dest) = (first, last) ICAO codes. Empty pair on empty input.
inline std::pair<std::string, std::string> parseHexdbRoute(const std::string& route) {
    if (route.empty()) return {"", ""};
    size_t first = route.find('-');
    if (first == std::string::npos) return {route, route};
    std::string origin = route.substr(0, first);
    size_t last = route.find_last_of('-');
    std::string dest = route.substr(last + 1);
    return {origin, dest};
}

// Airline ICAO code = first 3 chars of an airline callsign (letters). "" for
// tail-number callsigns (digits) or short callsigns.
inline std::string airlineCode(const std::string& callsign) {
    if (callsign.size() < 3) return "";
    for (int i = 0; i < 3; i++) {
        char c = callsign[i];
        if (c < 'A' || c > 'Z') return "";
    }
    return callsign.substr(0, 3);
}

// --- Display-range presets + radar zoom helpers ---

// Display-range presets in km, ascending (index 0 = nearest zoom, last = widest).
// The widest preset doubles as the fixed API reception radius.
inline constexpr double kRangePresets[] = {25.0, 50.0, 100.0};
inline constexpr int    kRangeCount = 3;

// Clamp idx+delta into [0, count-1]. Ladder semantics: no wrap at the ends.
inline int clampRangeIndex(int idx, int delta, int count) {
    int n = idx + delta;
    if (n < 0) n = 0;
    if (n > count - 1) n = count - 1;
    return n;
}

// True when an aircraft sits beyond the display range (-> draw it as a rim dot).
// Exactly on the boundary counts as in-range.
inline bool isOnRim(double distKm, double displayRangeKm) {
    return distKm > displayRangeKm;
}

// API query radius in nautical miles for a reception radius given in km, rounded up
// (1 NM = 1.852 km). Used to build the airplanes.live poll URL.
inline int queryRadiusNm(double maxPresetKm) {
    return (int)std::ceil(maxPresetKm / 1.852);
}
