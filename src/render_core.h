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
