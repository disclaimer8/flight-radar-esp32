#pragma once
#include <cstdint>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>
#include "flight_core.h"   // Aircraft, haversineKm

// Compact BLE wire protocol (little-endian; both ESP32 and host are LE).
static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "BLE wire format assumes little-endian");
constexpr uint8_t BLE_MAGIC0       = 0x46; // 'F'
constexpr uint8_t BLE_MAGIC1       = 0x52; // 'R'
constexpr uint8_t BLE_VERSION      = 3;
constexpr size_t  BLE_MAX_AIRCRAFT = 10;
constexpr size_t  BLE_HEADER_SIZE  = 12;
constexpr size_t  BLE_RECORD_SIZE  = 48;
constexpr size_t  BLE_MAX_PACKET   = BLE_HEADER_SIZE + BLE_MAX_AIRCRAFT * BLE_RECORD_SIZE; // 492

constexpr uint8_t BLE_FLAG_GROUND    = 0x01;
constexpr uint8_t BLE_FLAG_ALT_VALID = 0x02;
constexpr uint8_t BLE_FLAG_GS_VALID  = 0x04;
constexpr uint8_t BLE_FLAG_TRACK_VALID  = 0x08;
constexpr uint8_t BLE_FLAG_SQUAWK_VALID = 0x10;
constexpr uint8_t BLE_FLAG_MILITARY     = 0x20;

struct BlePacket {
    bool   ok = false;
    double centerLat = 0.0;
    double centerLon = 0.0;
    std::vector<Aircraft> aircraft;  // distKm filled, sorted nearest-first, capped to maxN
};

// Trim a fixed-width ASCII field (space-padded, possibly NUL-terminated).
inline std::string bleField(const uint8_t* p, size_t n) {
    std::string s(reinterpret_cast<const char*>(p), n);
    size_t z = s.find('\0');
    if (z != std::string::npos) s.resize(z);
    size_t a = s.find_first_not_of(' ');
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(' ');
    return s.substr(a, b - a + 1);
}

// Decode one binary packet. Returns ok=false (empty) on any validation failure.
inline BlePacket parseBlePacket(const uint8_t* buf, size_t len, size_t maxN, bool hideGround = false) {
    BlePacket out;
    if (!buf || len < BLE_HEADER_SIZE) return out;
    if (buf[0] != BLE_MAGIC0 || buf[1] != BLE_MAGIC1) return out;
    if (buf[2] != BLE_VERSION) return out;
    uint8_t count = buf[3];
    if (count > BLE_MAX_AIRCRAFT) return out;
    if (len != BLE_HEADER_SIZE + (size_t)count * BLE_RECORD_SIZE) return out;

    float clat, clon;
    std::memcpy(&clat, buf + 4, 4);
    std::memcpy(&clon, buf + 8, 4);
    out.centerLat = clat;
    out.centerLon = clon;

    for (uint8_t i = 0; i < count; i++) {
        const uint8_t* r = buf + BLE_HEADER_SIZE + (size_t)i * BLE_RECORD_SIZE;
        Aircraft ac;
        ac.callsign = bleField(r, 8);
        ac.type     = bleField(r + 8, 4);
        float lat, lon; int32_t altFt; int16_t gsKt;
        std::memcpy(&lat,   r + 12, 4);
        std::memcpy(&lon,   r + 16, 4);
        std::memcpy(&altFt, r + 20, 4);
        std::memcpy(&gsKt,  r + 24, 2);
        uint8_t flags = r[26];
        ac.lat = lat;
        ac.lon = lon;
        ac.onGround = (flags & BLE_FLAG_GROUND) != 0;
        if (hideGround && ac.onGround) continue; // drop ground traffic before sort/cap
        ac.altFt = (flags & BLE_FLAG_ALT_VALID) ? (double)altFt : NAN;
        ac.gsKt  = (flags & BLE_FLAG_GS_VALID)  ? (double)gsKt  : NAN;
        int16_t track; uint16_t squawk;
        std::memcpy(&track,  r + 28, 2);
        std::memcpy(&squawk, r + 30, 2);
        ac.track  = (flags & BLE_FLAG_TRACK_VALID)  ? (double)track : NAN;
        ac.squawk = (flags & BLE_FLAG_SQUAWK_VALID) ? (int)squawk   : 0;
        ac.isMilitary = (flags & BLE_FLAG_MILITARY) != 0;
        ac.registration = bleField(r + 32, 8);
        ac.origin       = bleField(r + 40, 4);
        ac.dest         = bleField(r + 44, 4);
        ac.distKm = haversineKm(out.centerLat, out.centerLon, ac.lat, ac.lon);
        out.aircraft.push_back(ac);
    }
    std::sort(out.aircraft.begin(), out.aircraft.end(),
              [](const Aircraft& a, const Aircraft& b) { return a.distKm < b.distKm; });
    if (out.aircraft.size() > maxN) out.aircraft.resize(maxN);
    out.ok = true;
    return out;
}
