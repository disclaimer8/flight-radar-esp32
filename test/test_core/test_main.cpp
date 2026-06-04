#include <unity.h>
#include "../../src/flight_core.h"
#include "../../src/render_core.h"
#include "../../src/coord_core.h"
#include "../../src/wifi_config_core.h"
#include "../../src/wifi_scan_core.h"
#include "../../src/ble_core.h"
#include "../../src/photo_core.h"
#include <cstring>

static const char* SAMPLE_JSON =
  "{\"ac\":["
    "{\"hex\":\"3c6abc\",\"flight\":\"DLH4AB  \",\"t\":\"A320\",\"alt_baro\":35000,\"gs\":453.6,\"lat\":48.10,\"lon\":11.00},"
    "{\"hex\":\"abc123\",\"flight\":\"BAW123  \",\"t\":\"B772\",\"alt_baro\":\"ground\",\"gs\":12.0,\"lat\":48.50,\"lon\":11.00},"
    "{\"hex\":\"def456\",\"flight\":\"RYR9XZ  \",\"t\":\"B738\",\"alt_baro\":12000,\"gs\":380.0,\"lat\":48.30,\"lon\":11.00}"
  "],\"msg\":\"No error\",\"now\":1.0,\"total\":3}";

void test_parseNearest_sorts_and_trims(void) {
    // Observer at 48.0/11.0. By latitude delta the order is:
    // DLH4AB (0.10), RYR9XZ (0.30), BAW123 (0.50).
    auto list = parseNearest(SAMPLE_JSON, 48.0, 11.0, 2);
    TEST_ASSERT_EQUAL_UINT32(2, list.size());          // trimmed to maxN
    TEST_ASSERT_EQUAL_STRING("DLH4AB", list[0].callsign.c_str()); // trimmed, nearest
    TEST_ASSERT_EQUAL_STRING("RYR9XZ", list[1].callsign.c_str());
    TEST_ASSERT_TRUE(list[0].distKm < list[1].distKm);
}

void test_parseNearest_handles_ground_and_fields(void) {
    auto list = parseNearest(SAMPLE_JSON, 48.0, 11.0, 5);
    TEST_ASSERT_EQUAL_UINT32(3, list.size());
    const Aircraft* gnd = nullptr;
    for (auto& a : list) if (a.callsign == "BAW123") gnd = &a;
    TEST_ASSERT_NOT_NULL(gnd);
    TEST_ASSERT_TRUE(gnd->onGround);
    TEST_ASSERT_EQUAL_STRING("B772", gnd->type.c_str());
}

void test_parseNearest_empty(void) {
    auto list = parseNearest("{\"ac\":[],\"total\":0}", 48.0, 11.0, 5);
    TEST_ASSERT_EQUAL_UINT32(0, list.size());
}

void test_ftToM(void) {
    TEST_ASSERT_FLOAT_WITHIN(0.5, 10668.0, ftToM(35000.0)); // 35000 ft ≈ 10668 m
    TEST_ASSERT_EQUAL_FLOAT(0.0, ftToM(0.0));
}

void test_ktToKmh(void) {
    TEST_ASSERT_FLOAT_WITHIN(0.1, 1.852, ktToKmh(1.0));
    TEST_ASSERT_FLOAT_WITHIN(1.0, 840.0, ktToKmh(453.6)); // ~454 kt ≈ 840 km/h
}

void test_haversineKm(void) {
    // 1 degree of longitude at the equator ≈ 111.19 km
    TEST_ASSERT_FLOAT_WITHIN(0.5, 111.19, haversineKm(0.0, 0.0, 0.0, 1.0));
    // Same point => 0
    TEST_ASSERT_FLOAT_WITHIN(0.01, 0.0, haversineKm(48.0, 11.0, 48.0, 11.0));
    // Munich area sanity: ~0.1 deg lat ≈ 11.1 km
    TEST_ASSERT_FLOAT_WITHIN(0.5, 11.12, haversineKm(48.0, 11.0, 48.1, 11.0));
}

static Aircraft mkAc(std::string cs, std::string t, double altFt,
                     bool gnd, double gsKt, double distKm) {
    Aircraft a;
    a.callsign = cs; a.type = t; a.altFt = altFt;
    a.onGround = gnd; a.gsKt = gsKt; a.distKm = distKm;
    return a;
}

void test_formatLine1_basic(void) {
    Aircraft a = mkAc("DLH4AB", "A320", 35000, false, 453.6, 12.0);
    std::string l1 = formatLine1(a);
    TEST_ASSERT_EQUAL_UINT32(16, l1.size());          // always exactly 16
    TEST_ASSERT_EQUAL_STRING("DLH4AB     12km ", l1.c_str());
}

void test_formatLine1_empty_callsign(void) {
    Aircraft a = mkAc("", "A320", 35000, false, 453.6, 5.0);
    std::string l1 = formatLine1(a);
    TEST_ASSERT_EQUAL_UINT32(16, l1.size());
    TEST_ASSERT_EQUAL_STRING("------      5km ", l1.c_str());
}

void test_formatLine2_basic(void) {
    Aircraft a = mkAc("DLH4AB", "A320", 35000, false, 453.6, 12.0);
    std::string l2 = formatLine2(a);            // 35000ft->10668m, 453.6kt->840km/h
    TEST_ASSERT_EQUAL_UINT32(16, l2.size());
    TEST_ASSERT_EQUAL_STRING("A320 10668m 840 ", l2.c_str());
}

void test_formatLine2_ground(void) {
    Aircraft a = mkAc("BAW123", "B772", NAN, true, 12.0, 30.0);
    std::string l2 = formatLine2(a);
    TEST_ASSERT_EQUAL_UINT32(16, l2.size());
    TEST_ASSERT_EQUAL_STRING("B772 GND 22     ", l2.c_str()); // 12kt->22km/h
}

void test_formatLine1_long_callsign(void) {
    Aircraft a = mkAc("LONGCALL123", "A320", 35000, false, 453.6, 12.0);
    std::string l1 = formatLine1(a);
    TEST_ASSERT_EQUAL_UINT32(16, l1.size());
    TEST_ASSERT_EQUAL_STRING("LONGCALL   12km ", l1.c_str()); // truncated to 8
}

void test_formatLine1_distance_clamped(void) {
    Aircraft a = mkAc("AAA", "A320", 35000, false, 453.6, 1500.0);
    std::string l1 = formatLine1(a);
    TEST_ASSERT_EQUAL_UINT32(16, l1.size());
    TEST_ASSERT_EQUAL_STRING("AAA       999km ", l1.c_str()); // clamped to 999
}

void test_formatLine1_stale_marker(void) {
    Aircraft a = mkAc("DLH4AB", "A320", 35000, false, 453.6, 12.0);
    std::string l1 = formatLine1(a, true);
    TEST_ASSERT_EQUAL_UINT32(16, l1.size());
    TEST_ASSERT_EQUAL_STRING("DLH4AB     12km*", l1.c_str()); // '*' in reserved last column
}

void test_formatLine2_nan_alt_and_speed(void) {
    Aircraft a = mkAc("XYZ", "A320", NAN, false, NAN, 5.0);
    std::string l2 = formatLine2(a);
    TEST_ASSERT_EQUAL_UINT32(16, l2.size());
    TEST_ASSERT_EQUAL_STRING("A320 --- ---    ", l2.c_str());
}

void test_formatLine2_negative_altitude(void) {
    Aircraft a = mkAc("XYZ", "B738", -1000.0, false, 100.0, 5.0);
    std::string l2 = formatLine2(a); // -1000ft -> -305m, 100kt -> 185km/h
    TEST_ASSERT_EQUAL_UINT32(16, l2.size());
    TEST_ASSERT_EQUAL_STRING("B738 -305m 185  ", l2.c_str());
}

void test_formatLine2_extreme_values_clamped(void) {
    Aircraft a = mkAc("XYZ", "A320", 400000.0, false, 20000.0, 5.0);
    std::string l2 = formatLine2(a); // clamp alt->99999m, spd->9999
    TEST_ASSERT_EQUAL_UINT32(16, l2.size());
    TEST_ASSERT_EQUAL_STRING("A320 99999m 9999", l2.c_str());
}

void test_parseNearest_malformed_json(void) {
    auto list = parseNearest("not valid json", 48.0, 11.0, 5);
    TEST_ASSERT_EQUAL_UINT32(0, list.size());
}

void test_polar_center_at_zero_distance(void) {
    ScreenPoint p = polarToXY(123.0, 0.0, 50.0, 120, 120, 96);
    TEST_ASSERT_EQUAL_INT(120, p.x);
    TEST_ASSERT_EQUAL_INT(120, p.y);
}

void test_polar_north_and_east_full_range(void) {
    ScreenPoint n = polarToXY(0.0, 50.0, 50.0, 120, 120, 96); // due north = up
    TEST_ASSERT_EQUAL_INT(120, n.x);
    TEST_ASSERT_EQUAL_INT(24,  n.y);
    ScreenPoint e = polarToXY(90.0, 50.0, 50.0, 120, 120, 96); // due east = right
    TEST_ASSERT_EQUAL_INT(216, e.x);
    TEST_ASSERT_EQUAL_INT(120, e.y);
}

void test_polar_clamps_beyond_range(void) {
    ScreenPoint far = polarToXY(90.0, 999.0, 50.0, 120, 120, 96); // 999km > 50km range
    TEST_ASSERT_EQUAL_INT(216, far.x); // pinned to ring edge
    TEST_ASSERT_EQUAL_INT(120, far.y);
}

void test_fmtDist(void) {
    TEST_ASSERT_EQUAL_STRING("6 km",   fmtDist(6.0).c_str());
    TEST_ASSERT_EQUAL_STRING("0 km",   fmtDist(-5.0).c_str());   // clamp low
    TEST_ASSERT_EQUAL_STRING("999 km", fmtDist(1500.0).c_str()); // clamp high
}

void test_fmtAlt(void) {
    Aircraft air = mkAc("X", "A320", 35000.0, false, 100.0, 5.0);
    TEST_ASSERT_EQUAL_STRING("10668m", fmtAlt(air).c_str()); // 35000ft -> 10668m
    Aircraft gnd = mkAc("X", "A320", NAN, true, 0.0, 5.0);
    TEST_ASSERT_EQUAL_STRING("GND", fmtAlt(gnd).c_str());
    Aircraft unk = mkAc("X", "A320", NAN, false, 0.0, 5.0);
    TEST_ASSERT_EQUAL_STRING("---", fmtAlt(unk).c_str());
}

void test_fmtSpeed(void) {
    Aircraft air = mkAc("X", "A320", 35000.0, false, 453.6, 5.0);
    TEST_ASSERT_EQUAL_STRING("840", fmtSpeed(air).c_str()); // 453.6kt -> 840km/h
    Aircraft unk = mkAc("X", "A320", 35000.0, false, NAN, 5.0);
    TEST_ASSERT_EQUAL_STRING("---", fmtSpeed(unk).c_str());
}

void test_compass_cardinals(void) {
    TEST_ASSERT_EQUAL_STRING("N",  compassPoint(0.0));
    TEST_ASSERT_EQUAL_STRING("NE", compassPoint(45.0));
    TEST_ASSERT_EQUAL_STRING("E",  compassPoint(90.0));
    TEST_ASSERT_EQUAL_STRING("S",  compassPoint(180.0));
    TEST_ASSERT_EQUAL_STRING("W",  compassPoint(270.0));
    TEST_ASSERT_EQUAL_STRING("NW", compassPoint(315.0));
}

void test_compass_wraps_and_rounds(void) {
    TEST_ASSERT_EQUAL_STRING("N", compassPoint(359.0)); // wraps to N
    TEST_ASSERT_EQUAL_STRING("N", compassPoint(10.0));  // rounds to N
    TEST_ASSERT_EQUAL_STRING("NE", compassPoint(30.0)); // rounds to NE
}

void test_compass_boundaries(void) {
    // Half-step rounding boundaries: lround(b/45.0) % 8
    // 22.5 -> lround(0.5)=1, %8=1 -> "NE"
    TEST_ASSERT_EQUAL_STRING("NE", compassPoint(22.5));
    // 67.5 -> lround(1.5)=2, %8=2 -> "E"
    TEST_ASSERT_EQUAL_STRING("E",  compassPoint(67.5));
    // 337.5 -> lround(7.5)=8, %8=0 -> "N"  (exercises modulo wrap on nonzero index)
    TEST_ASSERT_EQUAL_STRING("N",  compassPoint(337.5));
}

void test_polar_off_axis(void) {
    // 45deg bearing, half range: r=(25/50)*96=48; sin=cos=0.70711
    // x = lround(120 + 48*0.70711) = lround(153.94) = 154
    // y = lround(120 - 48*0.70711) = lround(86.06)  = 86
    ScreenPoint p = polarToXY(45.0, 25.0, 50.0, 120, 120, 96);
    TEST_ASSERT_EQUAL_INT(154, p.x);
    TEST_ASSERT_EQUAL_INT(86,  p.y);
}

void test_bearing_cardinals(void) {
    // From equator origin: north=0, east=90, south=180, west=270
    TEST_ASSERT_FLOAT_WITHIN(0.5, 0.0,   bearingDeg(0.0, 0.0,  1.0,  0.0));
    TEST_ASSERT_FLOAT_WITHIN(0.5, 90.0,  bearingDeg(0.0, 0.0,  0.0,  1.0));
    TEST_ASSERT_FLOAT_WITHIN(0.5, 180.0, bearingDeg(0.0, 0.0, -1.0,  0.0));
    TEST_ASSERT_FLOAT_WITHIN(0.5, 270.0, bearingDeg(0.0, 0.0,  0.0, -1.0));
}

void test_bearing_normalized_range(void) {
    double b = bearingDeg(48.0, 11.0, 47.5, 10.5); // southwest-ish
    TEST_ASSERT_TRUE(b >= 0.0 && b < 360.0);
    TEST_ASSERT_TRUE(b > 180.0 && b < 270.0);
}

// --- BLE packet test helpers ---
static void blePutF32(std::vector<uint8_t>& v, float f) {
    uint8_t b[4]; std::memcpy(b, &f, 4); v.insert(v.end(), b, b + 4);
}
static void blePutI32(std::vector<uint8_t>& v, int32_t x) {
    uint8_t b[4]; std::memcpy(b, &x, 4); v.insert(v.end(), b, b + 4);
}
static void blePutI16(std::vector<uint8_t>& v, int16_t x) {
    uint8_t b[2]; std::memcpy(b, &x, 2); v.insert(v.end(), b, b + 2);
}
static void blePutField(std::vector<uint8_t>& v, const char* s, size_t n) {
    size_t L = std::strlen(s);
    for (size_t i = 0; i < n; i++) v.push_back(i < L ? (uint8_t)s[i] : (uint8_t)' ');
}
static std::vector<uint8_t> bleHeader(uint8_t count, float clat, float clon) {
    std::vector<uint8_t> v;
    v.push_back(BLE_MAGIC0); v.push_back(BLE_MAGIC1);
    v.push_back(BLE_VERSION); v.push_back(count);
    blePutF32(v, clat); blePutF32(v, clon);
    return v;
}
static void bleAddRecord(std::vector<uint8_t>& v, const char* cs, const char* ty,
                         float lat, float lon, int32_t alt, int16_t gs, uint8_t flags,
                         int16_t track = 0, uint16_t squawk = 0,
                         const char* reg = "", const char* origin = "", const char* dest = "") {
    blePutField(v, cs, 8); blePutField(v, ty, 4);
    blePutF32(v, lat); blePutF32(v, lon);
    blePutI32(v, alt); blePutI16(v, gs);
    v.push_back(flags); v.push_back(0);   // flags + pad
    blePutI16(v, track);
    uint8_t b[2]; std::memcpy(b, &squawk, 2); v.insert(v.end(), b, b + 2); // u16 squawk LE
    blePutField(v, reg, 8); blePutField(v, origin, 4); blePutField(v, dest, 4);
}

void test_ble_valid_two_aircraft(void) {
    // Center 48,11. Record A is farther (48.5), B is nearer (48.1).
    std::vector<uint8_t> v = bleHeader(2, 48.0f, 11.0f);
    bleAddRecord(v, "DLH4AB", "A320", 48.5f, 11.0f, 35000,  453, BLE_FLAG_ALT_VALID | BLE_FLAG_GS_VALID);
    bleAddRecord(v, "RYR9XZ", "B738", 48.1f, 11.0f, 12000,  380, BLE_FLAG_ALT_VALID | BLE_FLAG_GS_VALID);
    BlePacket p = parseBlePacket(v.data(), v.size(), 5);
    TEST_ASSERT_TRUE(p.ok);
    TEST_ASSERT_FLOAT_WITHIN(0.001, 48.0, p.centerLat);
    TEST_ASSERT_FLOAT_WITHIN(0.001, 11.0, p.centerLon);
    TEST_ASSERT_EQUAL_UINT32(2, p.aircraft.size());
    TEST_ASSERT_EQUAL_STRING("RYR9XZ", p.aircraft[0].callsign.c_str()); // nearest first
    TEST_ASSERT_EQUAL_STRING("DLH4AB", p.aircraft[1].callsign.c_str());
    TEST_ASSERT_TRUE(p.aircraft[0].distKm < p.aircraft[1].distKm);
    TEST_ASSERT_EQUAL_STRING("B738", p.aircraft[0].type.c_str());
    TEST_ASSERT_FLOAT_WITHIN(0.5, 12000.0, p.aircraft[0].altFt);
    TEST_ASSERT_FLOAT_WITHIN(0.5, 380.0, p.aircraft[0].gsKt);
}

void test_ble_bad_magic(void) {
    std::vector<uint8_t> v = bleHeader(0, 48.0f, 11.0f);
    v[0] = 0x00;
    BlePacket p = parseBlePacket(v.data(), v.size(), 5);
    TEST_ASSERT_FALSE(p.ok);
}

void test_ble_bad_version(void) {
    std::vector<uint8_t> v = bleHeader(0, 48.0f, 11.0f);
    v[2] = 99;
    BlePacket p = parseBlePacket(v.data(), v.size(), 5);
    TEST_ASSERT_FALSE(p.ok);
}

void test_ble_count_overflow(void) {
    std::vector<uint8_t> v = bleHeader(17, 48.0f, 11.0f); // > BLE_MAX_AIRCRAFT
    BlePacket p = parseBlePacket(v.data(), v.size(), 5);
    TEST_ASSERT_FALSE(p.ok);
}

void test_ble_length_mismatch(void) {
    // count says 2 but only one record present
    std::vector<uint8_t> v = bleHeader(2, 48.0f, 11.0f);
    bleAddRecord(v, "AAA", "A320", 48.1f, 11.0f, 10000, 300, BLE_FLAG_ALT_VALID);
    BlePacket p = parseBlePacket(v.data(), v.size(), 5);
    TEST_ASSERT_FALSE(p.ok);
}

void test_ble_flags(void) {
    std::vector<uint8_t> v = bleHeader(1, 0.0f, 0.0f);
    bleAddRecord(v, "GND1", "B772", 0.0f, 0.1f, 0, 5, BLE_FLAG_GROUND); // ground, alt/gs invalid
    BlePacket p = parseBlePacket(v.data(), v.size(), 5);
    TEST_ASSERT_TRUE(p.ok);
    TEST_ASSERT_TRUE(p.aircraft[0].onGround);
    TEST_ASSERT_TRUE(std::isnan(p.aircraft[0].altFt));
    TEST_ASSERT_TRUE(std::isnan(p.aircraft[0].gsKt));
}

void test_ble_caps_to_maxN(void) {
    std::vector<uint8_t> v = bleHeader(3, 0.0f, 0.0f);
    bleAddRecord(v, "C", "A320", 0.0f, 3.0f, 1, 1, BLE_FLAG_ALT_VALID); // farthest
    bleAddRecord(v, "A", "A320", 0.0f, 1.0f, 1, 1, BLE_FLAG_ALT_VALID); // nearest
    bleAddRecord(v, "B", "A320", 0.0f, 2.0f, 1, 1, BLE_FLAG_ALT_VALID);
    BlePacket p = parseBlePacket(v.data(), v.size(), 2);
    TEST_ASSERT_TRUE(p.ok);
    TEST_ASSERT_EQUAL_UINT32(2, p.aircraft.size());        // capped
    TEST_ASSERT_EQUAL_STRING("A", p.aircraft[0].callsign.c_str()); // two nearest kept
    TEST_ASSERT_EQUAL_STRING("B", p.aircraft[1].callsign.c_str());
}

void test_parse_nearest_hides_ground(void) {
    // center 0,0. GND is nearest (0.1) but on the ground; A1 (0.2) and A2 (0.3) airborne.
    const char* json =
        "{\"ac\":["
        "{\"flight\":\"GND1\",\"t\":\"B772\",\"lat\":0.0,\"lon\":0.1,\"alt_baro\":\"ground\",\"gs\":3},"
        "{\"flight\":\"AIR1\",\"t\":\"A320\",\"lat\":0.0,\"lon\":0.2,\"alt_baro\":10000,\"gs\":300},"
        "{\"flight\":\"AIR2\",\"t\":\"A320\",\"lat\":0.0,\"lon\":0.3,\"alt_baro\":20000,\"gs\":400}"
        "]}";
    // hideGround = true: GND excluded, the two nearest AIRBORNE fill the slots.
    auto kept = parseNearest(json, 0.0, 0.0, 2, true);
    TEST_ASSERT_EQUAL_UINT32(2, kept.size());
    TEST_ASSERT_EQUAL_STRING("AIR1", kept[0].callsign.c_str());
    TEST_ASSERT_EQUAL_STRING("AIR2", kept[1].callsign.c_str());
    // hideGround = false: nearest 2 include the ground aircraft (current behavior).
    auto all = parseNearest(json, 0.0, 0.0, 2, false);
    TEST_ASSERT_EQUAL_UINT32(2, all.size());
    TEST_ASSERT_EQUAL_STRING("GND1", all[0].callsign.c_str());
    TEST_ASSERT_EQUAL_STRING("AIR1", all[1].callsign.c_str());
}

void test_ble_hides_ground(void) {
    // center 0,0. GND nearest (0.1, ground); A1 (0.2) and A2 (0.3) airborne.
    std::vector<uint8_t> v = bleHeader(3, 0.0f, 0.0f);
    bleAddRecord(v, "GND1", "B772", 0.0f, 0.1f, 0, 5, BLE_FLAG_GROUND);
    bleAddRecord(v, "AIR1", "A320", 0.0f, 0.2f, 10000, 300, BLE_FLAG_ALT_VALID | BLE_FLAG_GS_VALID);
    bleAddRecord(v, "AIR2", "A320", 0.0f, 0.3f, 20000, 400, BLE_FLAG_ALT_VALID | BLE_FLAG_GS_VALID);
    // hideGround = true: GND excluded; two nearest airborne fill the slots.
    BlePacket hid = parseBlePacket(v.data(), v.size(), 2, true);
    TEST_ASSERT_TRUE(hid.ok);
    TEST_ASSERT_EQUAL_UINT32(2, hid.aircraft.size());
    TEST_ASSERT_EQUAL_STRING("AIR1", hid.aircraft[0].callsign.c_str());
    TEST_ASSERT_EQUAL_STRING("AIR2", hid.aircraft[1].callsign.c_str());
    // hideGround = false: nearest 2 include the ground aircraft.
    BlePacket all = parseBlePacket(v.data(), v.size(), 2, false);
    TEST_ASSERT_TRUE(all.ok);
    TEST_ASSERT_EQUAL_STRING("GND1", all.aircraft[0].callsign.c_str());
    TEST_ASSERT_EQUAL_STRING("AIR1", all.aircraft[1].callsign.c_str());
}

void test_vector_end_cardinals(void) {
    // North-up: heading 0 = straight up (-y); 90 = right (+x).
    ScreenPoint up = vectorEnd(ScreenPoint{100, 100}, 0.0, 10.0);
    TEST_ASSERT_EQUAL_INT(100, up.x);
    TEST_ASSERT_EQUAL_INT(90, up.y);
    ScreenPoint right = vectorEnd(ScreenPoint{100, 100}, 90.0, 10.0);
    TEST_ASSERT_EQUAL_INT(110, right.x);
    TEST_ASSERT_EQUAL_INT(100, right.y);
}

void test_alt_band(void) {
    TEST_ASSERT_EQUAL_INT(0, altBand(NAN, false));
    TEST_ASSERT_EQUAL_INT(0, altBand(5000, true));
    TEST_ASSERT_EQUAL_INT(1, altBand(1500, false));
    TEST_ASSERT_EQUAL_INT(2, altBand(8000, false));
    TEST_ASSERT_EQUAL_INT(3, altBand(20000, false));
    TEST_ASSERT_EQUAL_INT(4, altBand(35000, false));
    TEST_ASSERT_EQUAL_INT(5, altBand(45000, false));
    // exact boundaries land in the upper band
    TEST_ASSERT_EQUAL_INT(2, altBand(3000, false));
    TEST_ASSERT_EQUAL_INT(3, altBand(10000, false));
    TEST_ASSERT_EQUAL_INT(4, altBand(25000, false));
    TEST_ASSERT_EQUAL_INT(5, altBand(40000, false));
}

void test_is_emergency_squawk(void) {
    TEST_ASSERT_TRUE(isEmergencySquawk(7500));
    TEST_ASSERT_TRUE(isEmergencySquawk(7600));
    TEST_ASSERT_TRUE(isEmergencySquawk(7700));
    TEST_ASSERT_FALSE(isEmergencySquawk(1200));
    TEST_ASSERT_FALSE(isEmergencySquawk(0));
}

void test_parse_nearest_track_squawk(void) {
    const char* json =
        "{\"ac\":["
        "{\"flight\":\"ABC\",\"t\":\"A320\",\"lat\":0.0,\"lon\":0.1,\"alt_baro\":10000,"
        "\"gs\":300,\"track\":275.4,\"squawk\":\"7700\"}"
        "]}";
    auto out = parseNearest(json, 0.0, 0.0, 5);
    TEST_ASSERT_EQUAL_UINT32(1, out.size());
    TEST_ASSERT_FLOAT_WITHIN(0.1, 275.4, out[0].track);
    TEST_ASSERT_EQUAL_INT(7700, out[0].squawk);
    // Missing track/squawk -> defaults (NAN track, 0 squawk).
    const char* json2 = "{\"ac\":[{\"flight\":\"X\",\"lat\":0.0,\"lon\":0.1}]}";
    auto out2 = parseNearest(json2, 0.0, 0.0, 5);
    TEST_ASSERT_TRUE(std::isnan(out2[0].track));
    TEST_ASSERT_EQUAL_INT(0, out2[0].squawk);
}

void test_ble_v2_track_squawk(void) {
    std::vector<uint8_t> v = bleHeader(1, 48.0f, 11.0f);
    bleAddRecord(v, "DLH", "A320", 48.1f, 11.0f, 35000, 450,
                 BLE_FLAG_ALT_VALID | BLE_FLAG_GS_VALID | BLE_FLAG_TRACK_VALID | BLE_FLAG_SQUAWK_VALID,
                 287, 7700);
    BlePacket p = parseBlePacket(v.data(), v.size(), 5);
    TEST_ASSERT_TRUE(p.ok);
    TEST_ASSERT_EQUAL_UINT32(1, p.aircraft.size());
    TEST_ASSERT_FLOAT_WITHIN(0.5, 287.0, p.aircraft[0].track);
    TEST_ASSERT_EQUAL_INT(7700, p.aircraft[0].squawk);
    // Invalid flags -> track NAN, squawk 0.
    std::vector<uint8_t> v2 = bleHeader(1, 0.0f, 0.0f);
    bleAddRecord(v2, "X", "B738", 0.0f, 0.1f, 1000, 100, BLE_FLAG_ALT_VALID, 123, 1200);
    BlePacket q = parseBlePacket(v2.data(), v2.size(), 5);
    TEST_ASSERT_TRUE(std::isnan(q.aircraft[0].track));
    TEST_ASSERT_EQUAL_INT(0, q.aircraft[0].squawk);
}

void test_parse_hexdb_route(void) {
    auto r = parseHexdbRoute("EGLL-KJFK");
    TEST_ASSERT_EQUAL_STRING("EGLL", r.first.c_str());
    TEST_ASSERT_EQUAL_STRING("KJFK", r.second.c_str());
    auto multi = parseHexdbRoute("EGLL-LEMD-EGLL"); // first->last
    TEST_ASSERT_EQUAL_STRING("EGLL", multi.first.c_str());
    TEST_ASSERT_EQUAL_STRING("EGLL", multi.second.c_str());
    auto empty = parseHexdbRoute("");
    TEST_ASSERT_EQUAL_STRING("", empty.first.c_str());
    TEST_ASSERT_EQUAL_STRING("", empty.second.c_str());
    auto one = parseHexdbRoute("EGLL");
    TEST_ASSERT_EQUAL_STRING("EGLL", one.first.c_str());
    TEST_ASSERT_EQUAL_STRING("EGLL", one.second.c_str());
}

void test_airline_code(void) {
    TEST_ASSERT_EQUAL_STRING("BAW", airlineCode("BAW117").c_str());
    TEST_ASSERT_EQUAL_STRING("DLH", airlineCode("DLH4AB").c_str());
    TEST_ASSERT_EQUAL_STRING("", airlineCode("N12345").c_str());
    TEST_ASSERT_EQUAL_STRING("", airlineCode("AB").c_str());
    TEST_ASSERT_EQUAL_STRING("", airlineCode("").c_str());
}

void test_parse_nearest_registration(void) {
    const char* json = "{\"ac\":[{\"flight\":\"BAW1\",\"r\":\"G-XLEA\",\"lat\":0.0,\"lon\":0.1}]}";
    auto out = parseNearest(json, 0.0, 0.0, 5);
    TEST_ASSERT_EQUAL_STRING("G-XLEA", out[0].registration.c_str());
    auto out2 = parseNearest("{\"ac\":[{\"flight\":\"X\",\"lat\":0.0,\"lon\":0.1}]}", 0.0, 0.0, 5);
    TEST_ASSERT_EQUAL_STRING("", out2[0].registration.c_str());
}

void test_ble_v3_route_registration(void) {
    std::vector<uint8_t> v = bleHeader(1, 48.0f, 11.0f);
    bleAddRecord(v, "BAW1", "A320", 48.1f, 11.0f, 35000, 450, BLE_FLAG_ALT_VALID,
                 0, 0, "G-XLEA", "EGLL", "KJFK");
    BlePacket p = parseBlePacket(v.data(), v.size(), 5);
    TEST_ASSERT_TRUE(p.ok);
    TEST_ASSERT_EQUAL_STRING("G-XLEA", p.aircraft[0].registration.c_str());
    TEST_ASSERT_EQUAL_STRING("EGLL", p.aircraft[0].origin.c_str());
    TEST_ASSERT_EQUAL_STRING("KJFK", p.aircraft[0].dest.c_str());
}

void test_clamp_range_index(void) {
    TEST_ASSERT_EQUAL_INT(0, clampRangeIndex(0, -1, 3)); // clamp at low end
    TEST_ASSERT_EQUAL_INT(2, clampRangeIndex(2, +1, 3)); // clamp at high end
    TEST_ASSERT_EQUAL_INT(1, clampRangeIndex(0, +1, 3)); // step up
    TEST_ASSERT_EQUAL_INT(1, clampRangeIndex(2, -1, 3)); // step down
    TEST_ASSERT_EQUAL_INT(0, clampRangeIndex(1, -1, 3)); // middle down
}

void test_is_on_rim(void) {
    TEST_ASSERT_FALSE(isOnRim(10.0, 25.0));  // inside the range
    TEST_ASSERT_FALSE(isOnRim(25.0, 25.0));  // exactly on the boundary = in range
    TEST_ASSERT_TRUE(isOnRim(30.0, 25.0));   // beyond the range
}

void test_query_radius_nm(void) {
    TEST_ASSERT_EQUAL_INT(54, queryRadiusNm(100.0)); // widest preset
    TEST_ASSERT_EQUAL_INT(27, queryRadiusNm(50.0));  // sanity vs today's 27 NM
}

void test_parse_lat_lon(void) {
    double la = 0, lo = 0;
    TEST_ASSERT_TRUE(parseLatLon("38.7677", "-9.3006", la, lo));
    TEST_ASSERT_FLOAT_WITHIN(0.0001, 38.7677, la);
    TEST_ASSERT_FLOAT_WITHIN(0.0001, -9.3006, lo);
    // boundaries accepted
    TEST_ASSERT_TRUE(parseLatLon("-90", "180", la, lo));
    TEST_ASSERT_TRUE(parseLatLon("90", "-180", la, lo));
    // out of range rejected
    double a = 1.5, b = 2.5;
    TEST_ASSERT_FALSE(parseLatLon("91", "0", a, b));
    TEST_ASSERT_FALSE(parseLatLon("0", "181", a, b));
    // garbage / empty / trailing junk rejected, out-params untouched
    TEST_ASSERT_FALSE(parseLatLon("abc", "0", a, b));
    TEST_ASSERT_FALSE(parseLatLon("", "0", a, b));
    TEST_ASSERT_FALSE(parseLatLon("38.7x", "0", a, b));
    TEST_ASSERT_FALSE(parseLatLon("nan", "0", a, b));
    TEST_ASSERT_FALSE(parseLatLon("0", "inf", a, b));
    TEST_ASSERT_FLOAT_WITHIN(0.0001, 1.5, a);
    TEST_ASSERT_FLOAT_WITHIN(0.0001, 2.5, b);
}

void test_parse_wifi_config(void) {
    uint8_t good[] = {0x57,0x43,0x01, 5,'M','y','N','e','t', 6,'s','e','c','r','e','t'};
    WifiConfig c = parseWifiConfig(good, sizeof(good));
    TEST_ASSERT_TRUE(c.ok);
    TEST_ASSERT_EQUAL_STRING("MyNet", c.ssid.c_str());
    TEST_ASSERT_EQUAL_STRING("secret", c.pass.c_str());

    uint8_t open[] = {0x57,0x43,0x01, 2,'A','P', 0};
    WifiConfig o = parseWifiConfig(open, sizeof(open));
    TEST_ASSERT_TRUE(o.ok);
    TEST_ASSERT_EQUAL_STRING("AP", o.ssid.c_str());
    TEST_ASSERT_EQUAL_STRING("", o.pass.c_str());

    uint8_t badmagic[] = {0x00,0x43,0x01, 2,'A','P', 0};
    TEST_ASSERT_FALSE(parseWifiConfig(badmagic, sizeof(badmagic)).ok);
    uint8_t badver[] = {0x57,0x43,0x09, 2,'A','P', 0};
    TEST_ASSERT_FALSE(parseWifiConfig(badver, sizeof(badver)).ok);
    uint8_t ssid0[] = {0x57,0x43,0x01, 0, 0};
    TEST_ASSERT_FALSE(parseWifiConfig(ssid0, sizeof(ssid0)).ok);
    uint8_t trunc[] = {0x57,0x43,0x01, 5,'M','y'};
    TEST_ASSERT_FALSE(parseWifiConfig(trunc, sizeof(trunc)).ok);
    uint8_t bigssid[] = {0x57,0x43,0x01, 33,'x'};
    TEST_ASSERT_FALSE(parseWifiConfig(bigssid, sizeof(bigssid)).ok);
    uint8_t bigpass[] = {0x57,0x43,0x01, 1,'A', 64,'x'};
    TEST_ASSERT_FALSE(parseWifiConfig(bigpass, sizeof(bigpass)).ok);

    // truncated pass: declares passLen 6 but only 2 pass bytes present
    uint8_t truncpass[] = {0x57,0x43,0x01, 1,'A', 6,'p','w'};
    TEST_ASSERT_FALSE(parseWifiConfig(truncpass, sizeof(truncpass)).ok);
}

void test_wifi_scan_request_parse() {
    uint8_t ok[] = {0x57, 0x53, 0x01};
    TEST_ASSERT_TRUE(isScanRequest(ok, 3));
    uint8_t okTrail[] = {0x57, 0x53, 0x01, 0xFF};   // trailing bytes tolerated
    TEST_ASSERT_TRUE(isScanRequest(okTrail, 4));
    uint8_t badMagic[] = {0x57, 0x43, 0x01};        // "WC" = wifi-config, not scan
    TEST_ASSERT_FALSE(isScanRequest(badMagic, 3));
    uint8_t badVer[] = {0x57, 0x53, 0x02};
    TEST_ASSERT_FALSE(isScanRequest(badVer, 3));
    TEST_ASSERT_FALSE(isScanRequest(ok, 2));        // truncated
    TEST_ASSERT_FALSE(isScanRequest(nullptr, 3));
}

void test_wifi_scan_record_encode() {
    uint8_t buf[WIFISCAN_REC_MAX];
    ScanNet n{"HomeNet", -62, true};
    size_t len = encodeScanRecord(buf, 3, 1, n);
    TEST_ASSERT_EQUAL(8 + 7, len);
    TEST_ASSERT_EQUAL_HEX8(0x57, buf[0]);                    // 'W'
    TEST_ASSERT_EQUAL_HEX8(0x4E, buf[1]);                    // 'N'
    TEST_ASSERT_EQUAL_HEX8(1, buf[2]);                       // version
    TEST_ASSERT_EQUAL_HEX8(3, buf[3]);                       // total
    TEST_ASSERT_EQUAL_HEX8(1, buf[4]);                       // index
    TEST_ASSERT_EQUAL_HEX8((uint8_t)(int8_t)-62, buf[5]);    // rssi as int8
    TEST_ASSERT_EQUAL_HEX8(1, buf[6]);                       // secured
    TEST_ASSERT_EQUAL_HEX8(7, buf[7]);                       // ssidLen
    TEST_ASSERT_EQUAL_MEMORY("HomeNet", buf + 8, 7);

    ScanNet maxSsid{std::string(32, 'a'), -50, false};
    TEST_ASSERT_EQUAL(40, encodeScanRecord(buf, 1, 0, maxSsid));
    ScanNet tooBig{std::string(33, 'a'), -50, false};
    TEST_ASSERT_EQUAL(0, encodeScanRecord(buf, 1, 0, tooBig));
    ScanNet empty{"", -50, false};
    TEST_ASSERT_EQUAL(0, encodeScanRecord(buf, 1, 0, empty));
}

void test_wifi_scan_empty_encode() {
    uint8_t buf[8];
    TEST_ASSERT_EQUAL(4, encodeScanEmpty(buf));
    TEST_ASSERT_EQUAL_HEX8(0x57, buf[0]);
    TEST_ASSERT_EQUAL_HEX8(0x4E, buf[1]);
    TEST_ASSERT_EQUAL_HEX8(1, buf[2]);
    TEST_ASSERT_EQUAL_HEX8(0, buf[3]);   // total=0 → none found
}

void test_parse_nearest_hex(void) {
    auto list = parseNearest(SAMPLE_JSON, 48.0, 11.0, 5);
    TEST_ASSERT_EQUAL_UINT32(3, list.size());
    TEST_ASSERT_EQUAL_STRING("3c6abc", list[0].hex.c_str());  // nearest = DLH4AB
}

void test_wifi_scan_dedup_sort_cap() {
    std::vector<ScanNet> in = {
        {"B", -80, true}, {"A", -60, false}, {"B", -50, true}, {"", -10, false},
    };
    auto out = dedupSortCap(in);
    TEST_ASSERT_EQUAL(2, out.size());
    TEST_ASSERT_EQUAL_STRING("B", out[0].ssid.c_str());  // strongest duplicate kept
    TEST_ASSERT_EQUAL(-50, out[0].rssi);
    TEST_ASSERT_EQUAL_STRING("A", out[1].ssid.c_str());  // sorted by RSSI desc

    std::vector<ScanNet> many;
    for (int i = 0; i < 20; i++)
        many.push_back({"n" + std::to_string(i), (int8_t)(-30 - i), false});
    TEST_ASSERT_EQUAL(15, dedupSortCap(many).size());    // capped
}

void test_parse_planespotters_photo(void) {
    // happy path: thumbnail_large preferred
    const char* ok =
      "{\"photos\":[{\"id\":\"1\",\"thumbnail\":{\"src\":\"https://t/small.jpg\"},"
      "\"thumbnail_large\":{\"src\":\"https://t/large.jpg\"},"
      "\"photographer\":\"Jane Doe\"}]}";
    PsPhoto p = parsePlanespottersPhoto(ok);
    TEST_ASSERT_TRUE(p.ok);
    TEST_ASSERT_EQUAL_STRING("https://t/large.jpg", p.url.c_str());
    TEST_ASSERT_EQUAL_STRING("Jane Doe", p.photographer.c_str());

    // fallback to thumbnail when thumbnail_large absent
    const char* fbk =
      "{\"photos\":[{\"thumbnail\":{\"src\":\"https://t/small.jpg\"},"
      "\"photographer\":\"X\"}]}";
    PsPhoto pf = parsePlanespottersPhoto(fbk);
    TEST_ASSERT_TRUE(pf.ok);
    TEST_ASSERT_EQUAL_STRING("https://t/small.jpg", pf.url.c_str());
}

void test_parse_planespotters_photo_misses(void) {
    TEST_ASSERT_FALSE(parsePlanespottersPhoto("{\"photos\":[]}").ok);   // no photos
    TEST_ASSERT_FALSE(parsePlanespottersPhoto("{}").ok);                // no key
    TEST_ASSERT_FALSE(parsePlanespottersPhoto("not json").ok);          // malformed
    TEST_ASSERT_FALSE(parsePlanespottersPhoto(
        "{\"photos\":[{\"photographer\":\"X\"}]}").ok);                 // no src at all
}

void test_pick_jpeg_scale_and_crop(void) {
    // largest divisor d in {1,2,4,8} with srcW/d>=240 AND srcH/d>=240, else 1
    TEST_ASSERT_EQUAL(1, pickJpegScale(400, 267));    // 1/2 would undershoot 240
    TEST_ASSERT_EQUAL(2, pickJpegScale(960, 640));
    TEST_ASSERT_EQUAL(4, pickJpegScale(2000, 1500));
    TEST_ASSERT_EQUAL(8, pickJpegScale(4000, 3000));
    TEST_ASSERT_EQUAL(1, pickJpegScale(200, 150));    // undersized -> letterbox
    // centering offsets (can be negative for letterbox)
    TEST_ASSERT_EQUAL(80, cropOffset(400));    // (400-240)/2
    TEST_ASSERT_EQUAL(0, cropOffset(240));
    TEST_ASSERT_EQUAL(-20, cropOffset(200));   // centers an undersized image
}

void setUp(void) {}
void tearDown(void) {}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_ftToM);
    RUN_TEST(test_ktToKmh);
    RUN_TEST(test_haversineKm);
    RUN_TEST(test_parseNearest_sorts_and_trims);
    RUN_TEST(test_parseNearest_handles_ground_and_fields);
    RUN_TEST(test_parseNearest_empty);
    RUN_TEST(test_formatLine1_basic);
    RUN_TEST(test_formatLine1_empty_callsign);
    RUN_TEST(test_formatLine2_basic);
    RUN_TEST(test_formatLine2_ground);
    RUN_TEST(test_formatLine1_long_callsign);
    RUN_TEST(test_formatLine1_distance_clamped);
    RUN_TEST(test_formatLine1_stale_marker);
    RUN_TEST(test_formatLine2_nan_alt_and_speed);
    RUN_TEST(test_formatLine2_negative_altitude);
    RUN_TEST(test_formatLine2_extreme_values_clamped);
    RUN_TEST(test_parseNearest_malformed_json);
    RUN_TEST(test_fmtDist);
    RUN_TEST(test_fmtAlt);
    RUN_TEST(test_fmtSpeed);
    RUN_TEST(test_compass_cardinals);
    RUN_TEST(test_compass_wraps_and_rounds);
    RUN_TEST(test_compass_boundaries);
    RUN_TEST(test_polar_center_at_zero_distance);
    RUN_TEST(test_polar_north_and_east_full_range);
    RUN_TEST(test_polar_clamps_beyond_range);
    RUN_TEST(test_polar_off_axis);
    RUN_TEST(test_bearing_cardinals);
    RUN_TEST(test_bearing_normalized_range);
    RUN_TEST(test_ble_valid_two_aircraft);
    RUN_TEST(test_ble_bad_magic);
    RUN_TEST(test_ble_bad_version);
    RUN_TEST(test_ble_count_overflow);
    RUN_TEST(test_ble_length_mismatch);
    RUN_TEST(test_ble_flags);
    RUN_TEST(test_ble_caps_to_maxN);
    RUN_TEST(test_parse_nearest_hides_ground);
    RUN_TEST(test_ble_hides_ground);
    RUN_TEST(test_vector_end_cardinals);
    RUN_TEST(test_alt_band);
    RUN_TEST(test_is_emergency_squawk);
    RUN_TEST(test_parse_nearest_track_squawk);
    RUN_TEST(test_parse_nearest_registration);
    RUN_TEST(test_ble_v2_track_squawk);
    RUN_TEST(test_ble_v3_route_registration);
    RUN_TEST(test_parse_hexdb_route);
    RUN_TEST(test_airline_code);
    RUN_TEST(test_clamp_range_index);
    RUN_TEST(test_is_on_rim);
    RUN_TEST(test_query_radius_nm);
    RUN_TEST(test_parse_lat_lon);
    RUN_TEST(test_parse_wifi_config);
    RUN_TEST(test_wifi_scan_request_parse);
    RUN_TEST(test_wifi_scan_record_encode);
    RUN_TEST(test_wifi_scan_empty_encode);
    RUN_TEST(test_wifi_scan_dedup_sort_cap);
    RUN_TEST(test_parse_nearest_hex);
    RUN_TEST(test_parse_planespotters_photo);
    RUN_TEST(test_parse_planespotters_photo_misses);
    RUN_TEST(test_pick_jpeg_scale_and_crop);
    return UNITY_END();
}
