#pragma once
#include <cmath>
#include <cstdio>
#include <string>
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

// 8-point compass rose label for a bearing in degrees.
inline const char* compassPoint(double bearing) {
    static const char* pts[8] = {"N","NE","E","SE","S","SW","W","NW"};
    double b = std::fmod(bearing + 360.0, 360.0);
    int idx = ((int)std::lround(b / 45.0)) % 8;
    return pts[idx];
}
