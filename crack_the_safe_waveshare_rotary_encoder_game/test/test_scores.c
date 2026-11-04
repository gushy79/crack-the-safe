/*
 * Host-side tests for main/scores.c (the Top-5 table, no hardware).
 * Build and run with `make -C test` (see test/Makefile).
 */
#include "game.h"
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

static score_entry_t E(uint32_t score, uint16_t levels)
{
    return (score_entry_t){ .score = score, .levels = levels };
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
    CHECK(scores_insert(&t, E(500, 1)) == 0);
    CHECK(t.count == 1);
    CHECK(t.e[0].score == 500 && t.e[0].levels == 1);
}

static void test_zero_score_is_rejected(void)
{
    score_table_t t;
    scores_clear(&t);
    CHECK(scores_insert(&t, E(0, 1)) == -1);
    CHECK(t.count == 0);
}

static void test_orders_best_first(void)
{
    score_table_t t;
    scores_clear(&t);
    CHECK(scores_insert(&t, E(300, 1)) == 0);
    CHECK(scores_insert(&t, E(900, 2)) == 0);     /* new best */
    CHECK(scores_insert(&t, E(600, 1)) == 1);     /* in the middle */
    CHECK(scores_insert(&t, E(100, 1)) == 3);     /* last */
    CHECK(t.count == 4);
    CHECK(t.e[0].score == 900 && t.e[1].score == 600 &&
          t.e[2].score == 300 && t.e[3].score == 100);
    CHECK(scores_valid(&t));
}

/* A full table: 100, 200 ... 100 x SCORES_MAX. */
static void fill(score_table_t *t)
{
    scores_clear(t);
    for (uint32_t k = 1; k <= SCORES_MAX; k++) {
        CHECK(scores_insert(t, E(k * 100, 1)) >= 0);
    }
    CHECK(t->count == SCORES_MAX);
}

static void test_full_table_drops_the_worst(void)
{
    score_table_t t;
    fill(&t);
    CHECK(t.e[SCORES_MAX - 1].score == 100);
    CHECK(scores_insert(&t, E(350, 2)) == SCORES_MAX - 3);    /* above 300, 200, 100 */
    CHECK(t.count == SCORES_MAX);
    CHECK(t.e[SCORES_MAX - 1].score == 200);                  /* 100 pushed out */
    CHECK(t.e[SCORES_MAX - 2].score == 300 && t.e[SCORES_MAX - 3].score == 350);
    CHECK(scores_valid(&t));
}

static void test_too_low_for_a_full_table_is_rejected(void)
{
    score_table_t t;
    fill(&t);
    const score_table_t before = t;
    CHECK(!scores_qualifies(&t, 100));                 /* ties the worst: out */
    CHECK(!scores_qualifies(&t, 50));
    CHECK(scores_insert(&t, E(100, 3)) == -1);
    CHECK(scores_insert(&t, E(50, 3)) == -1);
    CHECK(memcmp(&t, &before, sizeof t) == 0);         /* untouched */
    CHECK(scores_qualifies(&t, 101));
}

static void test_a_low_score_gets_its_true_place_past_the_visible_five(void)
{
    score_table_t t;
    fill(&t);
    /* 104th best: 103 scores are higher (100 x SCORES_MAX down to ...). */
    const uint32_t s = (uint32_t)(SCORES_MAX - 103) * 100 + 50;
    CHECK(scores_insert(&t, E(s, 2)) == 103);          /* rank 103 = the 104th place */
    CHECK(t.e[103].score == s && t.e[102].score > s && t.e[104].score < s);
    CHECK(103 >= SCORES_SHOWN);
    CHECK(scores_valid(&t));
}

static void test_equal_score_ranks_below_the_earlier_one(void)
{
    score_table_t t;
    scores_clear(&t);
    scores_insert(&t, E(400, 11));
    CHECK(scores_insert(&t, E(400, 22)) == 1);
    CHECK(t.e[0].levels == 11 && t.e[1].levels == 22);
    CHECK(scores_insert(&t, E(400, 33)) == 2);
    CHECK(t.e[2].levels == 33);
}

static void test_new_best_into_a_full_table(void)
{
    score_table_t t;
    fill(&t);
    CHECK(scores_insert(&t, E(999999, 8)) == 0);
    CHECK(t.count == SCORES_MAX);
    CHECK(t.e[0].score == 999999 && t.e[1].score == 100u * SCORES_MAX);
    CHECK(t.e[SCORES_MAX - 1].score == 200);
    CHECK(scores_valid(&t));
}

static void test_table_never_breaks_under_many_inserts(void)
{
    score_table_t t;
    scores_clear(&t);
    uint32_t x = 12345;
    for (int i = 0; i < 2000; i++) {
        x = x * 1664525u + 1013904223u;
        scores_insert(&t, E(x % 3000, (uint16_t)(1 + x % 30)));
        CHECK(scores_valid(&t));
    }
    CHECK(t.count == SCORES_MAX);
}

static void test_valid_rejects_corrupt_tables(void)
{
    score_table_t t;
    scores_clear(&t);
    scores_insert(&t, E(500, 1));
    scores_insert(&t, E(400, 1));
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

static void test_valid_rejects_implausible_entries(void)
{
    score_table_t t;
    scores_clear(&t);
    scores_insert(&t, E(500, GAME_LEVELS));       /* the extremes are fine */
    CHECK(scores_valid(&t));

    score_table_t bad = t;
    bad.e[0].levels = 0;
    CHECK(!scores_valid(&bad));
    bad.e[0].levels = SCORES_MAX_LEVELS + 1;
    CHECK(!scores_valid(&bad));
    bad.e[0].levels = 0xFFFF;
    CHECK(!scores_valid(&bad));

    bad = t;
    bad.e[0].left_s = SCORES_MAX_LEFT_S + 1;           /* longer than any clock */
    CHECK(!scores_valid(&bad));
}

static void test_run_text(void)
{
    char b[48];
    scores_fmt_run(E(1359, 3), b, sizeof b);
    CHECK(strcmp(b, "1,359 \xC2\xB7 3 lvls") == 0);
    scores_fmt_run(E(200, 1), b, sizeof b);
    CHECK(strcmp(b, "200 \xC2\xB7 1 lvl") == 0);
    scores_fmt_run(E(9000, 7), b, sizeof b);                 /* not a whole loop yet */
    CHECK(strstr(b, "loop") == NULL);
    scores_fmt_run(E(12345, GAME_LEVELS), b, sizeof b);
    CHECK(strcmp(b, "12,345 \xC2\xB7 8 lvls \xC2\xB7 1 loop") == 0);
    scores_fmt_run(E(99999, 2 * GAME_LEVELS + 3), b, sizeof b);
    CHECK(strcmp(b, "99,999 \xC2\xB7 19 lvls \xC2\xB7 2 loops") == 0);
    scores_fmt_run(E(99999, 19), b, 8);                      /* too small: truncated, terminated */
    CHECK(strlen(b) < 8);
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
    RUN(test_a_low_score_gets_its_true_place_past_the_visible_five);
    RUN(test_equal_score_ranks_below_the_earlier_one);
    RUN(test_new_best_into_a_full_table);
    RUN(test_table_never_breaks_under_many_inserts);
    RUN(test_valid_rejects_corrupt_tables);
    RUN(test_valid_rejects_implausible_entries);
    RUN(test_run_text);
    RUN(test_thousands_separators);
    RUN(test_thousands_truncates_safely);

    printf("\n%d checks, %d failed\n", s_checks, s_failed);
    return s_failed ? 1 : 0;
}
