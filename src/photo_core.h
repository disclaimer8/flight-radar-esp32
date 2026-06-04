#pragma once
#include <string>
#include <ArduinoJson.h>

// Planespotters photo metadata + display scale/crop math for the 240x240
// round screen. Arduino-free (ArduinoJson works host-side), host-tested.

struct PsPhoto {
    bool ok = false;
    std::string url;           // thumbnail_large preferred, thumbnail fallback
    std::string photographer;  // attribution (required by planespotters)
};

// Extract photos[0].thumbnail_large.src (fallback thumbnail.src) + photographer.
inline PsPhoto parsePlanespottersPhoto(const std::string& json) {
    PsPhoto r;
    JsonDocument doc;
    if (deserializeJson(doc, json)) return r;
    JsonVariantConst photos = doc["photos"];
    if (!photos.is<JsonArrayConst>() || photos.size() == 0) return r;
    JsonVariantConst p = photos[0];
    const char* src = p["thumbnail_large"]["src"].as<const char*>();
    if (!src) src = p["thumbnail"]["src"].as<const char*>();
    if (!src) return r;
    r.url = src;
    const char* ph = p["photographer"].as<const char*>();
    r.photographer = ph ? ph : "";
    r.ok = true;
    return r;
}

// Largest JPEGDEC divisor d in {1,2,4,8} whose scaled image still covers
// 240x240 in BOTH dimensions; 1 when even full size doesn't (letterbox).
inline int pickJpegScale(int srcW, int srcH) {
    for (int d = 8; d >= 2; d /= 2)
        if (srcW / d >= 240 && srcH / d >= 240) return d;
    return 1;
}

// Centering offset for one scaled dimension onto the 240px target.
// Positive = crop that many source px off the leading edge; negative =
// letterbox margin (image smaller than the screen).
inline int cropOffset(int scaledDim) { return (scaledDim - 240) / 2; }
