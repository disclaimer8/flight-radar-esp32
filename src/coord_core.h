#pragma once
#include <cstdlib>

// Parse two coordinate strings; on success write lat/lon and return true. Returns
// false (leaving lat/lon untouched) when either string is empty, non-numeric, has
// trailing garbage, or is out of range (lat [-90,90], lon [-180,180]).
inline bool parseLatLon(const char* latStr, const char* lonStr, double& lat, double& lon) {
    if (!latStr || !lonStr || latStr[0] == '\0' || lonStr[0] == '\0') return false;
    char* latEnd = nullptr;
    char* lonEnd = nullptr;
    double la = std::strtod(latStr, &latEnd);
    double lo = std::strtod(lonStr, &lonEnd);
    if (latEnd == latStr || lonEnd == lonStr) return false; // no digits consumed
    if (*latEnd != '\0' || *lonEnd != '\0') return false;   // trailing garbage
    if (la < -90.0 || la > 90.0) return false;
    if (lo < -180.0 || lo > 180.0) return false;
    lat = la;
    lon = lo;
    return true;
}
