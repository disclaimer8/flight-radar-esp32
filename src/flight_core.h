#pragma once
#include <cmath>

inline double ftToM(double ft)  { return ft * 0.3048; }
inline double ktToKmh(double kt) { return kt * 1.852; }

inline double haversineKm(double lat1, double lon1, double lat2, double lon2) {
    const double R = 6371.0; // mean Earth radius, km
    const double toRad = M_PI / 180.0;
    double dLat = (lat2 - lat1) * toRad;
    double dLon = (lon2 - lon1) * toRad;
    double a = std::sin(dLat / 2) * std::sin(dLat / 2) +
               std::cos(lat1 * toRad) * std::cos(lat2 * toRad) *
               std::sin(dLon / 2) * std::sin(dLon / 2);
    return R * 2 * std::atan2(std::sqrt(a), std::sqrt(1 - a));
}
