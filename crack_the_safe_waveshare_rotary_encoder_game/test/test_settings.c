/*
 * Host-side tests for the pure part of main/settings.h (haptic_scale).
 * Build and run with `make -C test`.
 */
#include "settings.h"

#include <stdio.h>

static int s_checks, s_failed;

#define CHECK(cond)                                                          \
    do {                                                                     \
        s_checks++;                                                          \
        if (!(cond)) {                                                       \
            s_failed++;                                                      \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
        }                                                                    \
    } while (0)

#define RUN(fn)                                                              \
    do {                                                                     \
        const int before = s_failed;                                         \
        fn();                                                                \
        printf("%s %s\n", s_failed == before ? "ok  " : "FAIL", #fn);        \
    } while (0)

static void test_full_strength_changes_nothing(void)
{
    for (int l = 0; l <= 127; l++) {
        CHECK(haptic_scale((uint8_t)l, HAPTIC_MAX) == l);
    }
}

static void test_silence_stays_silent(void)
{
    for (int s = HAPTIC_MIN; s <= HAPTIC_MAX; s++) {
        CHECK(haptic_scale(0, s) == 0);
    }
}

static void test_a_buzz_never_rounds_to_silence(void)
{
    for (int s = HAPTIC_MIN; s <= HAPTIC_MAX; s++) {
        CHECK(haptic_scale(1, s) >= 1);
    }
}

static void test_the_steps_are_40_to_100_percent(void)
{
    CHECK(haptic_scale(100, 1) == 40);
    CHECK(haptic_scale(100, 2) == 55);
    CHECK(haptic_scale(100, 3) == 70);
    CHECK(haptic_scale(100, 4) == 85);
    CHECK(haptic_scale(100, 5) == 100);
}

static void test_stronger_is_never_weaker(void)
{
    for (int l = 0; l <= 127; l++) {
        for (int s = HAPTIC_MIN; s < HAPTIC_MAX; s++) {
            CHECK(haptic_scale((uint8_t)l, s) <= haptic_scale((uint8_t)l, s + 1));
        }
    }
}

static void test_out_of_range_strength_is_clamped(void)
{
    CHECK(haptic_scale(100, -3) == haptic_scale(100, HAPTIC_MIN));
    CHECK(haptic_scale(100, 0) == haptic_scale(100, HAPTIC_MIN));
    CHECK(haptic_scale(100, 99) == haptic_scale(100, HAPTIC_MAX));
}

static void test_never_exceeds_the_input(void)
{
    for (int l = 0; l <= 127; l++) {
        for (int s = HAPTIC_MIN; s <= HAPTIC_MAX; s++) {
            CHECK(haptic_scale((uint8_t)l, s) <= l);
        }
    }
}

int main(void)
{
    RUN(test_full_strength_changes_nothing);
    RUN(test_silence_stays_silent);
    RUN(test_a_buzz_never_rounds_to_silence);
    RUN(test_the_steps_are_40_to_100_percent);
    RUN(test_stronger_is_never_weaker);
    RUN(test_out_of_range_strength_is_clamped);
    RUN(test_never_exceeds_the_input);
    printf("\n%d checks, %d failed\n", s_checks, s_failed);
    return s_failed ? 1 : 0;
}
