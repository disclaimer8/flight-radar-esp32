#pragma once
#include <cmath>
#include <ArduinoJson.h>
#include <vector>
#include <string>
#include <algorithm>

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

struct Aircraft {
    std::string callsign;  // trimmed "flight", "" if absent
    std::string type;      // "t", "" if absent
    double altFt = NAN;    // alt_baro numeric; NAN if missing/ground
    bool   onGround = false;
    double gsKt = NAN;     // ground speed knots; NAN if missing
    double lat = 0.0;
    double lon = 0.0;
    double distKm = 0.0;   // filled by parseNearest
};

inline std::string trimStr(const char* s) {
    if (!s) return "";
    std::string v(s);
    size_t a = v.find_first_not_of(" \t");
    size_t b = v.find_last_not_of(" \t");
    if (a == std::string::npos) return "";
    return v.substr(a, b - a + 1);
}

inline std::vector<Aircraft> parseNearest(const std::string& json,
                                          double myLat, double myLon,
                                          size_t maxN) {
    std::vector<Aircraft> out;

    JsonDocument filter;
    filter["ac"][0]["flight"] = true;
    filter["ac"][0]["t"] = true;
    filter["ac"][0]["alt_baro"] = true;
    filter["ac"][0]["gs"] = true;
    filter["ac"][0]["lat"] = true;
    filter["ac"][0]["lon"] = true;

    JsonDocument doc;
    DeserializationError err =
        deserializeJson(doc, json, DeserializationOption::Filter(filter));
    if (err) return out;

    for (JsonObject a : doc["ac"].as<JsonArray>()) {
        Aircraft ac;
        ac.callsign = trimStr(a["flight"].as<const char*>());
        ac.type     = trimStr(a["t"].as<const char*>());
        if (a["alt_baro"].is<const char*>()) {
            ac.onGround = (std::string(a["alt_baro"].as<const char*>()) == "ground");
        } else if (a["alt_baro"].is<double>()) {
            ac.altFt = a["alt_baro"].as<double>();
        }
        if (a["gs"].is<double>()) ac.gsKt = a["gs"].as<double>();
        ac.lat = a["lat"] | 0.0;
        ac.lon = a["lon"] | 0.0;
        ac.distKm = haversineKm(myLat, myLon, ac.lat, ac.lon);
        out.push_back(ac);
    }

    std::sort(out.begin(), out.end(),
              [](const Aircraft& x, const Aircraft& y) { return x.distKm < y.distKm; });
    if (out.size() > maxN) out.resize(maxN);
    return out;
}
