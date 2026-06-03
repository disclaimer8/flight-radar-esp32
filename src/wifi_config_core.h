#pragma once
#include <cstdint>
#include <cstddef>
#include <string>

// Wi-Fi provisioning packet (app -> device over BLE). Little-endian / byte fields.
constexpr uint8_t WIFICFG_MAGIC0  = 0x57; // 'W'
constexpr uint8_t WIFICFG_MAGIC1  = 0x43; // 'C'
constexpr uint8_t WIFICFG_VERSION = 1;
constexpr size_t  WIFICFG_MAX_SSID = 32;
constexpr size_t  WIFICFG_MAX_PASS = 63;

struct WifiConfig {
    bool ok = false;
    std::string ssid;
    std::string pass;
};

// Parse "WC" + ver + ssidLen + ssid + passLen + pass. Returns ok=false on wrong
// magic/version, ssidLen 0 or >32, passLen >63, or a truncated buffer.
inline WifiConfig parseWifiConfig(const uint8_t* buf, size_t len) {
    WifiConfig c;
    if (!buf || len < 4) return c;                       // need magic(2)+ver+ssidLen
    if (buf[0] != WIFICFG_MAGIC0 || buf[1] != WIFICFG_MAGIC1) return c;
    if (buf[2] != WIFICFG_VERSION) return c;
    size_t ssidLen = buf[3];
    if (ssidLen == 0 || ssidLen > WIFICFG_MAX_SSID) return c;
    if (len < 4 + ssidLen + 1) return c;                 // need ssid + passLen byte
    size_t passOff = 4 + ssidLen;
    size_t passLen = buf[passOff];
    if (passLen > WIFICFG_MAX_PASS) return c;
    if (len < passOff + 1 + passLen) return c;           // truncated pass
    c.ssid.assign(reinterpret_cast<const char*>(buf + 4), ssidLen);
    c.pass.assign(reinterpret_cast<const char*>(buf + passOff + 1), passLen);
    c.ok = true;
    return c;
}
