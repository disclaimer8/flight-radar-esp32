#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>

// Photo-over-BLE wire format (BLE characteristic f1a90005). Pull model:
//   PR (device -> phone, NOTIFY): "request photo for <key>"
//   PH (phone -> device, WRITE):  transfer header (total JPEG length + credit)
//   PD (phone -> device, WRITE):  one JPEG chunk
// Little-endian (both ends are LE). Mirrors the wifi_scan_core.h style.
constexpr uint8_t PHOTOBLE_MAGIC      = 0x50; // 'P'
constexpr uint8_t PHOTOBLE_T_REQ      = 0x52; // 'R'
constexpr uint8_t PHOTOBLE_T_HEADER   = 0x48; // 'H'
constexpr uint8_t PHOTOBLE_T_DATA     = 0x44; // 'D'
constexpr uint8_t PHOTOBLE_VERSION    = 1;
constexpr size_t  PHOTOBLE_MAX_KEY    = 11;     // registration/hex
constexpr size_t  PHOTOBLE_MAX_CRED   = 47;     // photographer string
constexpr size_t  PHOTOBLE_REQ_MAX    = 5 + PHOTOBLE_MAX_KEY;   // hdr(5)+key
constexpr uint32_t PHOTOBLE_MAX_IMG   = 48u * 1024u;            // sanity cap

// PR: 'P','R',ver,reqId,keyLen,key... . Returns bytes written, 0 if invalid.
// buf must hold at least PHOTOBLE_REQ_MAX bytes.
inline size_t buildPhotoReq(uint8_t* buf, uint8_t reqId, const std::string& key) {
    if (!buf || key.empty() || key.size() > PHOTOBLE_MAX_KEY) return 0;
    buf[0] = PHOTOBLE_MAGIC;
    buf[1] = PHOTOBLE_T_REQ;
    buf[2] = PHOTOBLE_VERSION;
    buf[3] = reqId;
    buf[4] = static_cast<uint8_t>(key.size());
    std::memcpy(buf + 5, key.data(), key.size());
    return 5 + key.size();
}

struct PhotoReq { bool ok = false; uint8_t reqId = 0; std::string key; };

inline PhotoReq parsePhotoReq(const uint8_t* buf, size_t len) {
    PhotoReq r;
    if (!buf || len < 5) return r;
    if (buf[0] != PHOTOBLE_MAGIC || buf[1] != PHOTOBLE_T_REQ) return r;
    if (buf[2] != PHOTOBLE_VERSION) return r;
    uint8_t keyLen = buf[4];
    if (keyLen == 0 || keyLen > PHOTOBLE_MAX_KEY || len < 5u + keyLen) return r;
    r.reqId = buf[3];
    r.key.assign(reinterpret_cast<const char*>(buf + 5), keyLen);
    r.ok = true;
    return r;
}

struct PhotoHeader {
    bool ok = false; uint8_t reqId = 0; uint32_t totalLen = 0; std::string credit;
};

// PH: 'P','H',ver,reqId,totalLen(u32 LE),credLen,cred...
inline PhotoHeader parsePhotoHeader(const uint8_t* buf, size_t len) {
    PhotoHeader h;
    if (!buf || len < 9) return h;
    if (buf[0] != PHOTOBLE_MAGIC || buf[1] != PHOTOBLE_T_HEADER) return h;
    if (buf[2] != PHOTOBLE_VERSION) return h;
    uint32_t total;
    std::memcpy(&total, buf + 4, 4);
    uint8_t credLen = buf[8];
    if (credLen > PHOTOBLE_MAX_CRED || len < 9u + credLen) return h;
    h.reqId = buf[3];
    h.totalLen = total;
    if (credLen) h.credit.assign(reinterpret_cast<const char*>(buf + 9), credLen);
    h.ok = true;
    return h;
}

struct PhotoChunk {
    bool ok = false; uint8_t reqId = 0; uint16_t seq = 0;
    const uint8_t* data = nullptr; size_t dataLen = 0;
};

// PD: 'P','D',ver,reqId,seq(u16 LE),bytes... . `data` points into `buf`.
inline PhotoChunk parsePhotoChunk(const uint8_t* buf, size_t len) {
    PhotoChunk c;
    if (!buf || len < 6) return c;
    if (buf[0] != PHOTOBLE_MAGIC || buf[1] != PHOTOBLE_T_DATA) return c;
    if (buf[2] != PHOTOBLE_VERSION) return c;
    uint16_t seq;
    std::memcpy(&seq, buf + 4, 2);
    c.reqId = buf[3];
    c.seq = seq;
    c.data = buf + 6;
    c.dataLen = len - 6;
    c.ok = true;
    return c;
}
