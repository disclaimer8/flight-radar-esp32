#pragma once
#include <cmath>
#include <cstdio>
#include <ArduinoJson.h>
#include <vector>
#include <string>
#include <algorithm>

constexpr size_t LCD_COLS     = 16;
constexpr int    CALLSIGN_MAX = 8;
constexpr int    TYPE_MAX     = 4;

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

inline std::string padTo16(std::string s) {
    if (s.size() > LCD_COLS) return s.substr(0, LCD_COLS);
    s.append(LCD_COLS - s.size(), ' ');
    return s;
}

inline std::string formatLine1(const Aircraft& ac, bool stale = false) {
    const int CONTENT = (int)LCD_COLS - 1; // reserve last column for status flag
    std::string left = ac.callsign.empty() ? std::string("------") : ac.callsign;
    if ((int)left.size() > CALLSIGN_MAX) left = left.substr(0, CALLSIGN_MAX);

    long km = std::lround(ac.distKm);
    if (km < 0)   km = 0;
    if (km > 999) km = 999;
    char dist[8];
    std::snprintf(dist, sizeof(dist), "%ldkm", km);
    std::string right(dist);

    int pad = CONTENT - (int)left.size() - (int)right.size();
    if (pad < 1) {
        int keep = CONTENT - (int)right.size() - 1;
        if (keep < 0) keep = 0;
        left = left.substr(0, keep);
        pad = 1;
    }
    std::string line = left + std::string(pad, ' ') + right;
    line += (stale ? '*' : ' ');
    return line;
}

inline std::string formatLine2(const Aircraft& ac) {
    std::string type = ac.type.empty() ? std::string("----") : ac.type;
    if ((int)type.size() > TYPE_MAX) type = type.substr(0, TYPE_MAX);

    std::string altStr;
    if (ac.onGround) altStr = "GND";
    else if (std::isnan(ac.altFt)) altStr = "---";
    else {
        long m = std::lround(ftToM(ac.altFt));
        if (m > 99999) m = 99999;
        if (m < -9999) m = -9999;
        char b[12];
        std::snprintf(b, sizeof(b), "%ldm", m);
        altStr = b;
    }

    std::string spdStr;
    if (std::isnan(ac.gsKt)) spdStr = "---";
    else {
        long s = std::lround(ktToKmh(ac.gsKt));
        if (s < 0)    s = 0;
        if (s > 9999) s = 9999;
        char b[12];
        std::snprintf(b, sizeof(b), "%ld", s);
        spdStr = b;
    }

    return padTo16(type + " " + altStr + " " + spdStr);
}
