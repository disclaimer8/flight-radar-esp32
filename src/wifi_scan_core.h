#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

// Wi-Fi scan-on-demand wire format (BLE characteristic f1a90004). The app
// writes a scan request; the device replies with one notify per network so
// every notify fits any negotiated MTU (iOS can be as low as 185).
constexpr uint8_t WIFISCAN_MAGIC0       = 0x57; // 'W'
constexpr uint8_t WIFISCAN_REQ_MAGIC1   = 0x53; // 'S' (request, app -> device)
constexpr uint8_t WIFISCAN_REC_MAGIC1   = 0x4E; // 'N' (record, device -> app)
constexpr uint8_t WIFISCAN_VERSION      = 1;
constexpr size_t  WIFISCAN_MAX_SSID     = 32;
constexpr size_t  WIFISCAN_MAX_NETWORKS = 15;
constexpr size_t  WIFISCAN_REC_MAX      = 8 + WIFISCAN_MAX_SSID; // 40 B

// Plain aggregate (no member initializers) so C++11 brace-init works in both
// the native test env and the ESP32 toolchain; producers set every field.
struct ScanNet {
    std::string ssid;
    int8_t rssi;
    bool secured;
};

// "WS" + ver. Trailing bytes are tolerated (future use).
inline bool isScanRequest(const uint8_t* buf, size_t len) {
    return buf && len >= 3 &&
           buf[0] == WIFISCAN_MAGIC0 && buf[1] == WIFISCAN_REQ_MAGIC1 &&
           buf[2] == WIFISCAN_VERSION;
}

// Drop hidden (empty-SSID) and oversize-SSID networks, dedup by SSID keeping
// the strongest RSSI, sort by RSSI descending, cap at WIFISCAN_MAX_NETWORKS.
inline std::vector<ScanNet> dedupSortCap(const std::vector<ScanNet>& in) {
    std::vector<ScanNet> out;
    for (const auto& n : in) {
        if (n.ssid.empty() || n.ssid.size() > WIFISCAN_MAX_SSID) continue;
        bool merged = false;
        for (auto& o : out) {
            if (o.ssid == n.ssid) {
                if (n.rssi > o.rssi) { o.rssi = n.rssi; o.secured = n.secured; }
                merged = true;
                break;
            }
        }
        if (!merged) out.push_back(n);
    }
    std::sort(out.begin(), out.end(),
              [](const ScanNet& a, const ScanNet& b) { return a.rssi > b.rssi; });
    if (out.size() > WIFISCAN_MAX_NETWORKS) out.resize(WIFISCAN_MAX_NETWORKS);
    return out;
}

// "WN" + ver + total + index + rssi(int8) + secured + ssidLen + ssid.
// Returns bytes written, 0 if the record is invalid. buf must hold
// WIFISCAN_REC_MAX bytes.
inline size_t encodeScanRecord(uint8_t* buf, uint8_t total, uint8_t index,
                               const ScanNet& n) {
    if (!buf || n.ssid.empty() || n.ssid.size() > WIFISCAN_MAX_SSID) return 0;
    buf[0] = WIFISCAN_MAGIC0;
    buf[1] = WIFISCAN_REC_MAGIC1;
    buf[2] = WIFISCAN_VERSION;
    buf[3] = total;
    buf[4] = index;
    buf[5] = static_cast<uint8_t>(n.rssi);
    buf[6] = n.secured ? 1 : 0;
    buf[7] = static_cast<uint8_t>(n.ssid.size());
    std::memcpy(buf + 8, n.ssid.data(), n.ssid.size());
    return 8 + n.ssid.size();
}

// 4-byte "no networks found" notify: "WN" + ver + total=0.
inline size_t encodeScanEmpty(uint8_t* buf) {
    buf[0] = WIFISCAN_MAGIC0;
    buf[1] = WIFISCAN_REC_MAGIC1;
    buf[2] = WIFISCAN_VERSION;
    buf[3] = 0;
    return 4;
}
