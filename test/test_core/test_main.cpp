#include <unity.h>
#include "../../src/flight_core.h"

void test_ftToM(void) {
    TEST_ASSERT_FLOAT_WITHIN(0.5, 10668.0, ftToM(35000.0)); // 35000 ft ≈ 10668 m
    TEST_ASSERT_EQUAL_FLOAT(0.0, ftToM(0.0));
}

void test_ktToKmh(void) {
    TEST_ASSERT_FLOAT_WITHIN(0.1, 1.852, ktToKmh(1.0));
    TEST_ASSERT_FLOAT_WITHIN(1.0, 840.0, ktToKmh(453.6)); // ~454 kt ≈ 840 km/h
}

void setUp(void) {}
void tearDown(void) {}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_ftToM);
    RUN_TEST(test_ktToKmh);
    return UNITY_END();
}
