#include <unity.h>
#include "../../src/flight_core.h"

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
    return UNITY_END();
}
