/*
 * Host-side tests for main/scores.c (the Top-5 table, no hardware).
 * Build and run with `make -C test` (see test/Makefile).
 */
#include "scores.h"

#include <stdio.h>
#include <string.h>

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

static score_entry_t E(uint32_t score, uint8_t level, uint16_t left_s)
{
    return (score_entry_t){ .score = score, .level = level, .left_s = left_s };
}

static void test_empty_table(void)
{
    score_table_t t;
    scores_clear(&t);
    CHECK(t.count == 0);
    CHECK(scores_valid(&t));
    CHECK(scores_qualifies(&t, 1));
    CHECK(!scores_qualifies(&t, 0));        /* a zero score never counts */
}

static void test_first_entry_is_rank_zero(void)
{
    score_table_t t;
    scores_clear(&t);
    CHECK(scores_insert(&t, E(500, 1, 50)) == 0);
    CHECK(t.count == 1);
    CHECK(t.e[0].score == 500 && t.e[0].level == 1 && t.e[0].left_s == 50);
}

static void test_zero_score_is_rejected(void)
{
    score_table_t t;
    scores_clear(&t);
    CHECK(scores_insert(&t, E(0, 1, 0)) == -1);
    CHECK(t.count == 0);
}

static void test_orders_best_first(void)
{
    score_table_t t;
    scores_clear(&t);
    CHECK(scores_insert(&t, E(300, 1, 30)) == 0);
    CHECK(scores_insert(&t, E(900, 2, 45)) == 0);     /* new best */
    CHECK(scores_insert(&t, E(600, 1, 60)) == 1);     /* in the middle */
    CHECK(scores_insert(&t, E(100, 1, 10)) == 3);     /* last */
    CHECK(t.count == 4);
    CHECK(t.e[0].score == 900 && t.e[1].score == 600 &&
          t.e[2].score == 300 && t.e[3].score == 100);
    CHECK(scores_valid(&t));
}

static void test_full_table_drops_the_worst(void)
{
    score_table_t t;
    scores_clear(&t);
    for (uint32_t s = 100; s <= 500; s += 100) {
        CHECK(scores_insert(&t, E(s, 1, 1)) >= 0);
    }
    CHECK(t.count == SCORES_MAX);
    CHECK(t.e[SCORES_MAX - 1].score == 100);
    CHECK(scores_insert(&t, E(350, 2, 2)) == 2);       /* pushes 100 out */
    CHECK(t.count == SCORES_MAX);
    CHECK(t.e[0].score == 500 && t.e[1].score == 400 && t.e[2].score == 350 &&
          t.e[3].score == 300 && t.e[4].score == 200);
    CHECK(scores_valid(&t));
}

static void test_too_low_for_a_full_table_is_rejected(void)
{
    score_table_t t;
    scores_clear(&t);
    for (uint32_t s = 100; s <= 500; s += 100) {
        scores_insert(&t, E(s, 1, 1));
    }
    const score_table_t before = t;
    CHECK(!scores_qualifies(&t, 100));                 /* ties the worst: out */
    CHECK(!scores_qualifies(&t, 50));
    CHECK(scores_insert(&t, E(100, 3, 3)) == -1);
    CHECK(scores_insert(&t, E(50, 3, 3)) == -1);
    CHECK(memcmp(&t, &before, sizeof t) == 0);         /* untouched */
    CHECK(scores_qualifies(&t, 101));
}

static void test_equal_score_ranks_below_the_earlier_one(void)
{
    score_table_t t;
    scores_clear(&t);
    scores_insert(&t, E(400, 1, 11));
    CHECK(scores_insert(&t, E(400, 2, 22)) == 1);
    CHECK(t.e[0].left_s == 11 && t.e[1].left_s == 22);
    CHECK(scores_insert(&t, E(400, 3, 33)) == 2);
    CHECK(t.e[2].left_s == 33);
}

static void test_new_best_into_a_full_table(void)
{
    score_table_t t;
    scores_clear(&t);
    for (uint32_t s = 100; s <= 500; s += 100) {
        scores_insert(&t, E(s, 1, 1));
    }
    CHECK(scores_insert(&t, E(9999, 8, 99)) == 0);
    CHECK(t.count == SCORES_MAX);
    CHECK(t.e[0].score == 9999 && t.e[4].score == 200);
    CHECK(scores_valid(&t));
}

static void test_table_never_breaks_under_many_inserts(void)
{
    score_table_t t;
    scores_clear(&t);
    uint32_t x = 12345;
    for (int i = 0; i < 2000; i++) {
        x = x * 1664525u + 1013904223u;
        scores_insert(&t, E(x % 3000, (uint8_t)(1 + x % 8), (uint16_t)(x % 300)));
        CHECK(scores_valid(&t));
    }
    CHECK(t.count == SCORES_MAX);
}

static void test_valid_rejects_corrupt_tables(void)
{
    score_table_t t;
    scores_clear(&t);
    scores_insert(&t, E(500, 1, 1));
    scores_insert(&t, E(400, 1, 1));
    CHECK(scores_valid(&t));

    score_table_t bad = t;
    bad.count = SCORES_MAX + 1;                        /* count out of range */
    CHECK(!scores_valid(&bad));

    bad = t;
    bad.e[1].score = 600;                              /* out of order */
    CHECK(!scores_valid(&bad));

    bad = t;
    bad.e[1].score = 0;                                /* an empty slot inside */
    CHECK(!scores_valid(&bad));

    memset(&bad, 0xFF, sizeof bad);                    /* erased flash */
    CHECK(!scores_valid(&bad));
}

static void test_thousands_separators(void)
{
    char b[16];
    scores_fmt_thousands(0, b, sizeof b);           CHECK(strcmp(b, "0") == 0);
    scores_fmt_thousands(7, b, sizeof b);           CHECK(strcmp(b, "7") == 0);
    scores_fmt_thousands(999, b, sizeof b);         CHECK(strcmp(b, "999") == 0);
    scores_fmt_thousands(1000, b, sizeof b);        CHECK(strcmp(b, "1,000") == 0);
    scores_fmt_thousands(1920, b, sizeof b);        CHECK(strcmp(b, "1,920") == 0);
    scores_fmt_thousands(24000, b, sizeof b);       CHECK(strcmp(b, "24,000") == 0);
    scores_fmt_thousands(1234567, b, sizeof b);     CHECK(strcmp(b, "1,234,567") == 0);
    scores_fmt_thousands(4294967295u, b, sizeof b); CHECK(strcmp(b, "4,294,967,295") == 0);
}

static void test_thousands_truncates_safely(void)
{
    char b[8];
    memset(b, 'x', sizeof b);
    scores_fmt_thousands(1234567, b, 4);            /* "1,2" fits, always terminated */
    CHECK(strlen(b) < 4);
    CHECK(b[0] == '1');
    scores_fmt_thousands(5, b, 1);
    CHECK(b[0] == '\0');
    scores_fmt_thousands(5, b, 0);                  /* must not write at all */
}

int main(void)
{
    RUN(test_empty_table);
    RUN(test_first_entry_is_rank_zero);
    RUN(test_zero_score_is_rejected);
    RUN(test_orders_best_first);
    RUN(test_full_table_drops_the_worst);
    RUN(test_too_low_for_a_full_table_is_rejected);
    RUN(test_equal_score_ranks_below_the_earlier_one);
    RUN(test_new_best_into_a_full_table);
    RUN(test_table_never_breaks_under_many_inserts);
    RUN(test_valid_rejects_corrupt_tables);
    RUN(test_thousands_separators);
    RUN(test_thousands_truncates_safely);

    printf("\n%d checks, %d failed\n", s_checks, s_failed);
    return s_failed ? 1 : 0;
}
