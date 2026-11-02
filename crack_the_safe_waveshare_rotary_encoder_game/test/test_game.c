/*
 * Host-side tests for main/game.c (no LVGL, no hardware).
 * Build and run with `make -C test` (see test/Makefile).
 */
#include "game.h"

#include <stdio.h>
#include <stdlib.h>

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

#define T0 1000u   /* start time; non-zero because 0 means "unset" in game.c */

/* Detents far enough apart (>= 500 ms) that the speed estimate is 0, so a hit
 * on the number is always felt. */
#define SLOW_MS 600u

static uint32_t s_now;

static unsigned turn(game_t *g, int dir)
{
    s_now += SLOW_MS;
    return game_rotate(g, dir, s_now);
}

/* Title -> rules -> first code dealt at time T0. */
static void begin(game_t *g, uint32_t seed)
{
    game_init(g, seed);
    s_now = T0;
    game_rotate(g, 1, s_now);
    game_start(g, s_now);
}

/* Turn slowly in the stage direction until the number is reached. */
static unsigned go_to_target(game_t *g)
{
    const int dir = game_stage_dir(g);
    unsigned ev = 0;
    for (int n = game_distance(g); n > 0; n--) {
        ev |= turn(g, dir);
    }
    return ev;
}

/* Reach the number and reverse on it to lock it in (or open the safe). */
static unsigned solve_stage(game_t *g)
{
    unsigned ev = go_to_target(g);
    ev |= turn(g, -game_stage_dir(g));
    return ev;
}

/* ---- startup ---------------------------------------------------------- */

static void test_init_is_title(void)
{
    game_t g;
    game_init(&g, 1);
    CHECK(g.mode == GAME_TITLE);
    CHECK(g.level == 0);
    CHECK(g.ticks == 0);
    CHECK(game_distance(&g) == -1);
    CHECK(game_time_left_ms(&g, 5000) == game_level_ms(1));
    CHECK(game_elapsed_s(&g, 5000) == 0);
}

static void test_zero_seed_is_usable(void)
{
    game_t g;
    begin(&g, 0);
    CHECK(g.mode == GAME_PLAY);
    CHECK(g.code[0] < GAME_POSITIONS);
}

static void test_title_turn_shows_rules_then_start_deals(void)
{
    game_t g;
    game_init(&g, 7);
    CHECK(game_rotate(&g, 3, T0) == GAME_EV_INSTRUCTIONS);
    CHECK(g.mode == GAME_INSTRUCTIONS);
    CHECK(g.ticks == 0);                       /* the dial did not move */
    CHECK(game_rotate(&g, 3, T0 + 10) == GAME_EV_NONE);  /* rules ignore the knob */
    CHECK(g.mode == GAME_INSTRUCTIONS);
    CHECK(game_start(&g, T0 + 20) == GAME_EV_STARTED);
    CHECK(g.mode == GAME_PLAY);
    CHECK(g.level == 1);
    CHECK(g.attempts == 1);
    CHECK(g.stage == 0);
    CHECK(g.started_ms == T0 + 20);
}

static void test_title_tap_shows_rules(void)
{
    game_t g;
    game_init(&g, 7);
    CHECK(game_tap(&g, T0) == GAME_EV_INSTRUCTIONS);
    CHECK(g.mode == GAME_INSTRUCTIONS);
    CHECK(game_tap(&g, T0 + 10) == GAME_EV_NONE);   /* only Start moves on */
}

static void test_start_only_works_from_rules(void)
{
    game_t g;
    game_init(&g, 7);
    CHECK(game_start(&g, T0) == GAME_EV_NONE);
    CHECK(g.mode == GAME_TITLE);
}

/* ---- dial and dealing ------------------------------------------------- */

static void test_dial_value_counts_down_and_wraps(void)
{
    game_t g;
    game_init(&g, 1);
    CHECK(game_dial_value(&g) == 0);
    g.ticks = 1;
    CHECK(game_dial_value(&g) == GAME_POSITIONS - 1);
    g.ticks = -1;
    CHECK(game_dial_value(&g) == 1);
    g.ticks = GAME_POSITIONS;
    CHECK(game_dial_value(&g) == 0);
    g.ticks = -3 * GAME_POSITIONS - 5;
    CHECK(game_dial_value(&g) == 5);
}

static void test_stage_direction_alternates(void)
{
    game_t g;
    game_init(&g, 1);
    g.stage = 0;
    CHECK(game_stage_dir(&g) == +1);
    g.stage = 1;
    CHECK(game_stage_dir(&g) == -1);
    g.stage = 2;
    CHECK(game_stage_dir(&g) == +1);
}

static int circ(int a, int b)
{
    int d = abs(a - b) % GAME_POSITIONS;
    return d < GAME_POSITIONS - d ? d : GAME_POSITIONS - d;
}

static void test_codes_in_range_and_spaced(void)
{
    for (uint32_t seed = 1; seed <= 500; seed++) {
        game_t g;
        begin(&g, seed);
        int prev = 0;   /* the dial rests at 0 for the first game */
        for (int k = 0; k < GAME_CODE_LEN; k++) {
            CHECK(g.code[k] < GAME_POSITIONS);
            CHECK(circ(g.code[k], prev) >= 5);
            prev = g.code[k];
        }
    }
}

static void test_same_seed_same_code(void)
{
    game_t a, b;
    begin(&a, 1234);
    begin(&b, 1234);
    for (int k = 0; k < GAME_CODE_LEN; k++) {
        CHECK(a.code[k] == b.code[k]);
    }
}

static void test_distance_counts_down_to_the_number(void)
{
    game_t g;
    begin(&g, 42);
    const int dir = game_stage_dir(&g);
    int d = game_distance(&g);
    CHECK(d >= 5 && d < GAME_POSITIONS);
    while (d > 0) {
        g.ticks += dir;
        const int nd = game_distance(&g);
        CHECK(nd == d - 1);
        d = nd;
    }
    CHECK(game_dial_value(&g) == g.code[0]);
}

/* ---- play: hits, locks, failures ------------------------------------- */

static void test_slow_turn_hits_the_number(void)
{
    game_t g;
    begin(&g, 42);
    unsigned ev = go_to_target(&g);
    CHECK(ev & GAME_EV_HIT);
    CHECK(!(ev & GAME_EV_WRONG));
    CHECK(g.on_target);
    CHECK(g.hit_felt);
    CHECK(g.mode == GAME_PLAY);
    CHECK(game_distance(&g) == 0);
}

static void test_reverse_on_number_locks_it(void)
{
    game_t g;
    begin(&g, 42);
    unsigned ev = solve_stage(&g);
    CHECK(ev & GAME_EV_LOCKED);
    CHECK(g.mode == GAME_PLAY);
    CHECK(g.stage == 1);
    CHECK(g.locked == 1);
    CHECK(!g.on_target);
    CHECK(game_stage_dir(&g) == -1);
}

static void test_going_past_the_number_loses(void)
{
    game_t g;
    begin(&g, 42);
    go_to_target(&g);
    unsigned ev = turn(&g, game_stage_dir(&g));
    CHECK(ev & GAME_EV_WRONG);
    CHECK(!(ev & GAME_EV_TIMEOUT));
    CHECK(g.mode == GAME_WRONG);
    CHECK(!g.timed_out);
}

static void test_reversing_off_target_loses_after_committing(void)
{
    game_t g;
    begin(&g, 42);
    const int dir = game_stage_dir(&g);
    CHECK(game_distance(&g) > 3);          /* two detents cannot reach it */
    turn(&g, dir);
    turn(&g, dir);
    unsigned ev = turn(&g, -dir);
    CHECK(ev & GAME_EV_WRONG);
    CHECK(g.mode == GAME_WRONG);
}

static void test_small_wobble_is_forgiven(void)
{
    game_t g;
    begin(&g, 42);
    const int dir = game_stage_dir(&g);
    turn(&g, dir);
    unsigned ev = turn(&g, -dir);
    CHECK(ev == GAME_EV_NONE);
    CHECK(g.mode == GAME_PLAY);
    CHECK(g.travel == 0);
    /* ... and an immediate extra reversal at travel 0 is forgiven too. */
    CHECK(turn(&g, -dir) == GAME_EV_NONE);
    CHECK(g.mode == GAME_PLAY);
}

static void test_reverse_on_second_stage_needs_the_second_number(void)
{
    game_t g;
    begin(&g, 42);
    solve_stage(&g);
    CHECK(g.stage == 1);
    /* Commit to the left, then reverse short of the number. */
    CHECK(game_distance(&g) > 3);
    turn(&g, -1);
    turn(&g, -1);
    CHECK(turn(&g, +1) & GAME_EV_WRONG);
}

static void test_full_solve_opens_the_safe_on_reverse(void)
{
    game_t g;
    begin(&g, 99);
    unsigned ev = 0;
    for (int k = 0; k < GAME_CODE_LEN; k++) {
        CHECK(g.mode == GAME_PLAY);
        CHECK(g.stage == k);
        ev = solve_stage(&g);
    }
    CHECK(ev & GAME_EV_UNLOCKED);
    CHECK(g.mode == GAME_UNLOCKED);
    CHECK(g.locked == GAME_CODE_LEN);
    CHECK(g.finished_ms != 0);
    CHECK(game_distance(&g) == -1);
}

static void test_last_number_opens_after_dwell(void)
{
    game_t g;
    begin(&g, 99);
    for (int k = 0; k < GAME_CODE_LEN - 1; k++) {
        solve_stage(&g);
    }
    go_to_target(&g);
    CHECK(g.on_target);
    CHECK(game_tick(&g, g.last_turn_ms + GAME_OPEN_DWELL_MS - 1) == GAME_EV_NONE);
    CHECK(g.mode == GAME_PLAY);
    CHECK(game_tick(&g, g.last_turn_ms + GAME_OPEN_DWELL_MS) == GAME_EV_UNLOCKED);
    CHECK(g.mode == GAME_UNLOCKED);
}

static void test_dwell_on_an_earlier_number_does_not_open(void)
{
    game_t g;
    begin(&g, 99);
    go_to_target(&g);                      /* stage 0 of 3 */
    CHECK(game_tick(&g, g.last_turn_ms + 10 * GAME_OPEN_DWELL_MS) == GAME_EV_NONE);
    CHECK(g.mode == GAME_PLAY);
}

static void test_fast_turn_arrives_unfelt_but_still_counts(void)
{
    game_t g;
    begin(&g, 42);
    game_rotate(&g, 0, s_now);             /* stamp last_turn_ms */
    const int dir = game_stage_dir(&g);
    const int n = game_distance(&g);
    unsigned ev = game_rotate(&g, dir * n, s_now + 10);   /* one fast batch */
    CHECK(!(ev & GAME_EV_HIT));            /* no click ... */
    CHECK(g.on_target);                    /* ... but resting on the number */
    CHECK(!g.hit_felt);
    CHECK(g.speed_dps >= GAME_FEEL_FAST_DPS);
    /* Reversing on an unfelt number still locks it. */
    CHECK(game_rotate(&g, -dir, s_now + 700) & GAME_EV_LOCKED);
}

static void test_fast_spin_past_the_number_still_loses(void)
{
    game_t g;
    begin(&g, 42);
    game_rotate(&g, 0, s_now);
    const int dir = game_stage_dir(&g);
    unsigned ev = game_rotate(&g, dir * (game_distance(&g) + 3), s_now + 10);
    CHECK(ev & GAME_EV_WRONG);
    CHECK(g.mode == GAME_WRONG);
}

static void test_input_ignored_while_wrong(void)
{
    game_t g;
    begin(&g, 42);
    go_to_target(&g);
    turn(&g, game_stage_dir(&g));
    CHECK(g.mode == GAME_WRONG);
    const int32_t ticks = g.ticks;
    CHECK(turn(&g, 1) == GAME_EV_NONE);
    CHECK(g.ticks == ticks);
}

/* ---- clock ------------------------------------------------------------ */

static void test_level_table_and_clamp(void)
{
    CHECK(game_level_ms(1) == 300000u);
    CHECK(game_level_ms(2) == 150000u);
    CHECK(game_level_ms(3) == 60000u);
    CHECK(game_level_ms(4) == 30000u);
    for (int l = 5; l <= GAME_LEVELS; l++) {
        CHECK(game_level_ms(l) == 20000u);
    }
    CHECK(game_level_ms(0) == game_level_ms(1));
    CHECK(game_level_ms(-5) == game_level_ms(1));
    CHECK(game_level_ms(GAME_LEVELS + 10) == game_level_ms(GAME_LEVELS));
}

static void test_time_left_counts_down(void)
{
    game_t g;
    begin(&g, 5);
    CHECK(game_time_left_ms(&g, T0) == 300000u);
    CHECK(game_time_left_ms(&g, T0 + 1000) == 299000u);
    CHECK(game_time_left_ms(&g, T0 + 300000u) == 0);
    CHECK(game_time_left_ms(&g, T0 + 999999u) == 0);
    CHECK(game_elapsed_s(&g, T0 + 12345) == 12);
}

static void test_timeout_loses_with_flag(void)
{
    game_t g;
    begin(&g, 5);
    CHECK(game_tick(&g, T0 + 299999u) == GAME_EV_NONE);
    unsigned ev = game_tick(&g, T0 + 300000u);
    CHECK(ev & GAME_EV_WRONG);
    CHECK(ev & GAME_EV_TIMEOUT);
    CHECK(g.mode == GAME_WRONG);
    CHECK(g.timed_out);
}

static void test_clock_freezes_when_the_safe_opens(void)
{
    game_t g;
    begin(&g, 99);
    for (int k = 0; k < GAME_CODE_LEN; k++) {
        solve_stage(&g);
    }
    CHECK(g.mode == GAME_UNLOCKED);
    const uint32_t left = game_time_left_ms(&g, s_now);
    CHECK(left > 0);
    CHECK(game_time_left_ms(&g, s_now + 100000) == left);
    CHECK(game_elapsed_s(&g, s_now + 100000) == game_elapsed_s(&g, s_now));
}

/* ---- after a loss / win ---------------------------------------------- */

static void test_wrong_hold_then_new_game_same_level(void)
{
    game_t g;
    begin(&g, 42);
    g.level = 3;
    go_to_target(&g);
    turn(&g, game_stage_dir(&g));
    CHECK(g.mode == GAME_WRONG);
    const uint32_t t = g.mode_since_ms;
    CHECK(game_tick(&g, t + GAME_WRONG_HOLD_MS - 1) == GAME_EV_NONE);
    CHECK(g.mode == GAME_WRONG);
    unsigned ev = game_tick(&g, t + GAME_WRONG_HOLD_MS);
    CHECK(ev & GAME_EV_RESUMED);
    CHECK(ev & GAME_EV_STARTED);
    CHECK(g.mode == GAME_PLAY);
    CHECK(g.level == 3);                   /* a loss replays the level */
    CHECK(g.ticks == 0);                   /* dial back at 0 */
    CHECK(g.stage == 0);
    CHECK(g.timed_out == false);
    CHECK(game_time_left_ms(&g, t + GAME_WRONG_HOLD_MS) == game_level_ms(3));
}

static void test_next_level_after_a_win_and_wraparound(void)
{
    game_t g;
    begin(&g, 99);
    for (int k = 0; k < GAME_CODE_LEN; k++) {
        solve_stage(&g);
    }
    CHECK(g.mode == GAME_UNLOCKED);
    CHECK(g.level == 1);
    CHECK(game_next_level(&g) == 2);
    g.level = GAME_LEVELS;
    CHECK(game_next_level(&g) == 1);       /* after the last level, loop */
    g.level = 1;

    /* Locked out right after opening ... */
    CHECK(game_rotate(&g, 1, g.mode_since_ms + GAME_REPLAY_LOCKOUT_MS - 1) == GAME_EV_NONE);
    CHECK(game_tap(&g, g.mode_since_ms + GAME_REPLAY_LOCKOUT_MS - 1) == GAME_EV_NONE);
    CHECK(g.mode == GAME_UNLOCKED);
    /* ... then a turn starts the next level. */
    CHECK(game_rotate(&g, 1, g.mode_since_ms + GAME_REPLAY_LOCKOUT_MS) == GAME_EV_STARTED);
    CHECK(g.mode == GAME_PLAY);
    CHECK(g.level == 2);
}

static void test_tap_after_lockout_starts_next_level(void)
{
    game_t g;
    begin(&g, 99);
    for (int k = 0; k < GAME_CODE_LEN; k++) {
        solve_stage(&g);
    }
    CHECK(game_tap(&g, g.mode_since_ms + GAME_REPLAY_LOCKOUT_MS) == GAME_EV_STARTED);
    CHECK(g.level == 2);
    CHECK(g.finished_ms == 0);
}

static void test_game_new_goes_to_level_one(void)
{
    game_t g;
    begin(&g, 99);
    g.level = 5;
    unsigned ev = game_new(&g, 50000);
    CHECK(ev & GAME_EV_RESET);
    CHECK(ev & GAME_EV_STARTED);
    CHECK(g.mode == GAME_PLAY);
    CHECK(g.level == 1);
    CHECK(g.ticks == 0);
    CHECK(g.attempts == 1);
}

static void test_reset_returns_to_title(void)
{
    game_t g;
    begin(&g, 99);
    go_to_target(&g);
    CHECK(game_reset(&g, 60000) == GAME_EV_RESET);
    CHECK(g.mode == GAME_TITLE);
    CHECK(g.level == 0);
    CHECK(g.ticks == 0);
    CHECK(g.locked == 0);
    /* The first game after a reset is level 1 again. */
    game_rotate(&g, 1, 61000);
    game_start(&g, 62000);
    CHECK(g.level == 1);
}

/* ---- score ------------------------------------------------------------ */

static void test_score_is_level_times_tenths_left(void)
{
    CHECK(game_score(1, 192000u) == 1920u);          /* 3:12 left on level 1 */
    CHECK(game_score(3, 45000u) == 3u * 450u);
    CHECK(game_score(8, 20000u) == 8u * 200u);
    CHECK(game_score(2, 99u) == 0u);                 /* under a tenth: nothing */
    CHECK(game_score(2, 100u) == 2u);
    CHECK(game_score(1, 0) == 0u);
}

static void test_score_clamps_the_level(void)
{
    CHECK(game_score(0, 10000u) == game_score(1, 10000u));
    CHECK(game_score(-3, 10000u) == game_score(1, 10000u));
    CHECK(game_score(GAME_LEVELS + 5, 10000u) == game_score(GAME_LEVELS, 10000u));
}

static void test_more_time_left_never_scores_less(void)
{
    for (int lv = 1; lv <= GAME_LEVELS; lv++) {
        uint32_t prev = 0;
        for (uint32_t ms = 0; ms <= game_level_ms(lv); ms += 250) {
            const uint32_t s = game_score(lv, ms);
            CHECK(s >= prev);
            prev = s;
        }
    }
}

static void test_higher_level_beats_lower_at_the_same_time_left(void)
{
    CHECK(game_score(5, 15000u) > game_score(4, 15000u));
}

static void test_current_score_only_when_unlocked(void)
{
    game_t g;
    begin(&g, 99);
    CHECK(game_current_score(&g, s_now) == 0);       /* playing */
    for (int k = 0; k < GAME_CODE_LEN; k++) {
        solve_stage(&g);
    }
    CHECK(g.mode == GAME_UNLOCKED);
    const uint32_t s = game_current_score(&g, s_now);
    CHECK(s == game_score(g.level, game_time_left_ms(&g, s_now)));
    CHECK(s > 0);
    CHECK(game_current_score(&g, s_now + 100000) == s);   /* frozen at the opening */
}

static void test_no_score_for_a_loss(void)
{
    game_t g;
    begin(&g, 42);
    go_to_target(&g);
    turn(&g, game_stage_dir(&g));
    CHECK(g.mode == GAME_WRONG);
    CHECK(game_current_score(&g, s_now) == 0);
}

/* ---- pause ------------------------------------------------------------ */

static void test_pause_stops_the_clock(void)
{
    game_t g;
    begin(&g, 5);
    game_pause(&g, T0 + 10000);
    game_pause(&g, T0 + 20000);            /* a second pause changes nothing */
    game_resume(&g, T0 + 70000);           /* 60 s away */
    CHECK(!g.paused);
    CHECK(game_time_left_ms(&g, T0 + 70000) == 300000u - 10000u);
    game_resume(&g, T0 + 90000);           /* resume without pause: no-op */
    CHECK(game_time_left_ms(&g, T0 + 70000) == 300000u - 10000u);
}

static void test_pause_holds_the_red_timer(void)
{
    game_t g;
    begin(&g, 42);
    go_to_target(&g);
    turn(&g, game_stage_dir(&g));
    CHECK(g.mode == GAME_WRONG);
    const uint32_t t = s_now;
    game_pause(&g, t + 1000);
    game_resume(&g, t + 51000);
    /* 1 s of the 2 s hold had passed; 1 s more is still to run. */
    CHECK(game_tick(&g, t + 51000 + 999) == GAME_EV_NONE);
    CHECK(game_tick(&g, t + 51000 + 1000) & GAME_EV_RESUMED);
}

int main(void)
{
    RUN(test_init_is_title);
    RUN(test_zero_seed_is_usable);
    RUN(test_title_turn_shows_rules_then_start_deals);
    RUN(test_title_tap_shows_rules);
    RUN(test_start_only_works_from_rules);
    RUN(test_dial_value_counts_down_and_wraps);
    RUN(test_stage_direction_alternates);
    RUN(test_codes_in_range_and_spaced);
    RUN(test_same_seed_same_code);
    RUN(test_distance_counts_down_to_the_number);
    RUN(test_slow_turn_hits_the_number);
    RUN(test_reverse_on_number_locks_it);
    RUN(test_going_past_the_number_loses);
    RUN(test_reversing_off_target_loses_after_committing);
    RUN(test_small_wobble_is_forgiven);
    RUN(test_reverse_on_second_stage_needs_the_second_number);
    RUN(test_full_solve_opens_the_safe_on_reverse);
    RUN(test_last_number_opens_after_dwell);
    RUN(test_dwell_on_an_earlier_number_does_not_open);
    RUN(test_fast_turn_arrives_unfelt_but_still_counts);
    RUN(test_fast_spin_past_the_number_still_loses);
    RUN(test_input_ignored_while_wrong);
    RUN(test_level_table_and_clamp);
    RUN(test_time_left_counts_down);
    RUN(test_timeout_loses_with_flag);
    RUN(test_clock_freezes_when_the_safe_opens);
    RUN(test_wrong_hold_then_new_game_same_level);
    RUN(test_next_level_after_a_win_and_wraparound);
    RUN(test_tap_after_lockout_starts_next_level);
    RUN(test_game_new_goes_to_level_one);
    RUN(test_reset_returns_to_title);
    RUN(test_score_is_level_times_tenths_left);
    RUN(test_score_clamps_the_level);
    RUN(test_more_time_left_never_scores_less);
    RUN(test_higher_level_beats_lower_at_the_same_time_left);
    RUN(test_current_score_only_when_unlocked);
    RUN(test_no_score_for_a_loss);
    RUN(test_pause_stops_the_clock);
    RUN(test_pause_holds_the_red_timer);

    printf("\n%d checks, %d failed\n", s_checks, s_failed);
    return s_failed ? 1 : 0;
}
