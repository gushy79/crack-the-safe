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

static void test_wrong_hold_then_new_code_same_level_same_clock(void)
{
    game_t g;
    begin(&g, 42);
    g.level = 3;
    go_to_target(&g);
    turn(&g, game_stage_dir(&g));
    CHECK(g.mode == GAME_WRONG);
    const uint32_t t = g.mode_since_ms;
    const uint32_t at_miss = game_time_left_ms(&g, t);       /* frozen in the red */
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
    /* The clock went on from where the miss left it: the hold cost nothing. */
    CHECK(game_time_left_ms(&g, t + GAME_WRONG_HOLD_MS) == at_miss);
    CHECK(at_miss > 0 && at_miss <= game_level_ms(3));
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

/* ---- rules: Lives, Zen, and the split random streams ------------------ */

/* Overshoot the number: a loss (no clock cost beyond the turns themselves). */
static unsigned lose(game_t *g)
{
    go_to_target(g);
    return turn(g, game_stage_dir(g));
}

/* Sit out the red hold. */
static unsigned hold_red(game_t *g)
{
    s_now += GAME_WRONG_HOLD_MS;
    return game_tick(g, s_now);
}

static void begin_rules(game_t *g, uint32_t seed, game_rules_t r)
{
    game_init(g, seed);
    game_set_rules(g, r);
    s_now = T0;
    game_rotate(g, 1, s_now);
    game_start(g, s_now);
}

static void test_classic_is_the_default_and_has_no_lives(void)
{
    game_t g;
    begin(&g, 5);
    CHECK(g.rules == GAME_RULES_CLASSIC);
    CHECK(g.lives == 0);
    CHECK(game_timed(&g));
}

static void test_lives_start_full(void)
{
    game_t g;
    begin_rules(&g, 5, GAME_RULES_LIVES);
    CHECK(g.lives == GAME_LIVES);
}

static void test_lives_loss_keeps_the_same_code_and_costs_a_life(void)
{
    game_t g;
    begin_rules(&g, 5, GAME_RULES_LIVES);
    uint8_t code[GAME_CODE_LEN];
    for (int i = 0; i < GAME_CODE_LEN; i++) {
        code[i] = g.code[i];
    }
    CHECK(lose(&g) & GAME_EV_WRONG);
    CHECK(g.lives == GAME_LIVES - 1);
    const unsigned ev = hold_red(&g);
    CHECK(ev & GAME_EV_RETRY);
    CHECK(ev & GAME_EV_RESUMED);
    CHECK(!(ev & GAME_EV_STARTED));
    CHECK(g.mode == GAME_PLAY);
    for (int i = 0; i < GAME_CODE_LEN; i++) {
        CHECK(g.code[i] == code[i]);
    }
    CHECK(g.stage == 0 && g.locked == 0);
    CHECK(g.level == 1);
    CHECK(g.attempts == 2);
}

static void test_lives_retry_is_playable_and_far_enough_from_number_one(void)
{
    game_t g;
    begin_rules(&g, 11, GAME_RULES_LIVES);
    lose(&g);
    hold_red(&g);
    CHECK(game_distance(&g) > 0);      /* not already sitting on number 1 */
    for (int i = 0; i < GAME_CODE_LEN - 1; i++) {
        solve_stage(&g);
    }
    CHECK(solve_stage(&g) & GAME_EV_UNLOCKED);
}

static void test_lives_red_hold_costs_no_time(void)
{
    game_t g;
    begin_rules(&g, 5, GAME_RULES_LIVES);
    lose(&g);
    const uint32_t frozen = game_time_left_ms(&g, s_now);
    hold_red(&g);
    CHECK(game_time_left_ms(&g, s_now) == frozen);
    s_now += 1000;
    CHECK(game_time_left_ms(&g, s_now) == frozen - 1000);
}

static void test_lives_out_of_lives_ends_the_game(void)
{
    game_t g;
    begin_rules(&g, 5, GAME_RULES_LIVES);
    for (int n = 0; n < GAME_LIVES - 1; n++) {
        lose(&g);
        CHECK(hold_red(&g) & GAME_EV_RETRY);
    }
    CHECK(g.lives == 1);
    lose(&g);
    CHECK(g.lives == 0);
    const unsigned ev = hold_red(&g);
    CHECK(ev & GAME_EV_OVER);          /* it ends: nothing refills the lives */
    CHECK(!(ev & (GAME_EV_STARTED | GAME_EV_RETRY)));
    CHECK(g.mode == GAME_TITLE);
    CHECK(g.rules == GAME_RULES_LIVES);        /* the chosen rules stay */
}

static void test_lives_a_win_keeps_the_lives_left(void)
{
    game_t g;
    begin_rules(&g, 5, GAME_RULES_LIVES);
    lose(&g);
    hold_red(&g);
    for (int i = 0; i < GAME_CODE_LEN; i++) {
        solve_stage(&g);
    }
    CHECK(g.mode == GAME_UNLOCKED);
    CHECK(g.lives == GAME_LIVES - 1);
    s_now += GAME_REPLAY_LOCKOUT_MS;
    game_tap(&g, s_now);
    CHECK(g.mode == GAME_PLAY);
    CHECK(g.level == 2);
    CHECK(g.lives == GAME_LIVES - 1);
}

static void test_lives_timeout_ends_the_game_whatever_is_left(void)
{
    game_t g;
    begin_rules(&g, 5, GAME_RULES_LIVES);
    s_now += game_level_ms(1) + 1;
    CHECK(game_tick(&g, s_now) & GAME_EV_TIMEOUT);
    CHECK(g.lives == 0);
    const unsigned ev = hold_red(&g);
    CHECK(ev & GAME_EV_OVER);          /* the game ends, not a retry */
    CHECK(!(ev & GAME_EV_RETRY));
    CHECK(g.mode == GAME_TITLE);
}

static void test_lives_new_game_restores_full_lives_and_level_one(void)
{
    game_t g;
    begin_rules(&g, 5, GAME_RULES_LIVES);
    lose(&g);
    hold_red(&g);
    game_new(&g, s_now);
    CHECK(g.rules == GAME_RULES_LIVES);
    CHECK(g.lives == GAME_LIVES);
    CHECK(g.level == 1);
    CHECK(g.mode == GAME_PLAY);
}

static void open_next(game_t *g)
{
    for (int i = 0; i < GAME_CODE_LEN; i++) {
        solve_stage(g);
    }
    CHECK(g->mode == GAME_UNLOCKED);
}

static void test_run_totals_add_up_across_safes_and_loops(void)
{
    game_t g;
    begin_rules(&g, 11, GAME_RULES_LIVES);
    for (int n = 1; n <= GAME_LEVELS + 2; n++) {       /* a whole loop and two more */
        open_next(&g);
        CHECK(g.run_levels == n);
        CHECK(game_run_score(&g) / GAME_RUN_SAFE_POINTS == (uint32_t)n);
        s_now += GAME_REPLAY_LOCKOUT_MS;
        game_tap(&g, s_now);
    }
    CHECK(g.level == 3);                               /* round the loop and on */
    CHECK(g.loops == 1);                               /* one trip round the levels */
    CHECK(g.lives == GAME_LIVES);
}

static void test_run_totals_survive_lost_lives_and_end_with_the_game(void)
{
    game_t g;
    begin_rules(&g, 11, GAME_RULES_LIVES);
    open_next(&g);
    s_now += GAME_REPLAY_LOCKOUT_MS;
    game_tap(&g, s_now);
    const uint32_t kept = game_run_score(&g);
    CHECK(kept > 0 && g.run_levels == 1 && g.run_fails == 0);
    lose(&g);
    CHECK(g.run_fails == 1);
    CHECK(hold_red(&g) & GAME_EV_RETRY);               /* a life lost: the run goes on */
    CHECK(g.run_levels == 1);
    lose(&g);
    CHECK(hold_red(&g) & GAME_EV_RETRY);
    lose(&g);
    CHECK(g.lives == 0 && g.run_fails == 3);
    CHECK(game_run_over(&g));
    CHECK(g.run_levels == 1);                          /* still there for the caller */
    CHECK(hold_red(&g) & GAME_EV_OVER);
    CHECK(g.mode == GAME_TITLE && g.run_levels == 0 && g.run_fails == 0);
}

/* ---- the run score ----------------------------------------------------- */

static game_t synth(game_rules_t r, unsigned safes, uint32_t avg_speed, unsigned fails)
{
    game_t g;
    game_init(&g, 1);
    game_set_rules(&g, r);
    g.run_levels = (uint16_t)safes;
    g.run_speed = avg_speed * safes;
    g.run_fails = (uint16_t)fails;
    return g;
}

static void test_run_score_more_safes_always_wins(void)
{
    const game_rules_t modes[2] = { GAME_RULES_CLASSIC, GAME_RULES_LIVES };
    for (int m = 0; m < 2; m++) {
        for (unsigned n = 1; n < 300; n++) {
            /* The flawless, fastest run of n safes against the slowest, messiest of n + 1. */
            const game_t best = synth(modes[m], n, 10000u, 0);
            const game_t worst = synth(modes[m], n + 1, 0, 90);
            CHECK(game_run_score(&worst) > game_run_score(&best));
            CHECK(game_run_score(&best) / GAME_RUN_SAFE_POINTS == n);
        }
    }
}

static void test_run_score_same_safes_faster_wins(void)
{
    uint32_t prev = 0;
    for (uint32_t sp = 0; sp <= 10000; sp += 250) {
        const game_t g = synth(GAME_RULES_CLASSIC, 5, sp, 0);
        CHECK(game_run_score(&g) >= prev);
        prev = game_run_score(&g);
    }
    const game_t a = synth(GAME_RULES_LIVES, 6, 7000, 0);
    const game_t b = synth(GAME_RULES_LIVES, 6, 3000, 0);
    CHECK(game_run_score(&a) > game_run_score(&b));
}

static void test_run_score_lives_fails_cost_speed_classic_fails_do_not(void)
{
    const game_t clean = synth(GAME_RULES_LIVES, 4, 8000, 0);
    const game_t one = synth(GAME_RULES_LIVES, 4, 8000, 1);
    const game_t two = synth(GAME_RULES_LIVES, 4, 8000, 2);
    CHECK(game_run_score(&clean) > game_run_score(&one));
    CHECK(game_run_score(&one) > game_run_score(&two));
    CHECK(game_run_score(&one) == 4 * GAME_RUN_SAFE_POINTS + 8000 * GAME_FAIL_KEEP_PCT / 100);
    CHECK(game_run_score(&two) >= 4 * GAME_RUN_SAFE_POINTS);      /* never below the safes */

    const game_t c0 = synth(GAME_RULES_CLASSIC, 4, 8000, 0);
    const game_t c9 = synth(GAME_RULES_CLASSIC, 4, 8000, 9);
    CHECK(game_run_score(&c0) == game_run_score(&c9));
}

static void test_run_score_zero_without_safes_and_in_zen(void)
{
    game_t g = synth(GAME_RULES_CLASSIC, 0, 0, 3);
    CHECK(game_run_score(&g) == 0);
    g = synth(GAME_RULES_ZEN, 5, 9000, 0);
    CHECK(game_run_score(&g) == 0);
    g = synth(GAME_RULES_LIVES, 7, 99999, 0);          /* nonsense speed is clamped */
    CHECK(game_run_score(&g) == 7 * GAME_RUN_SAFE_POINTS + GAME_RUN_SAFE_POINTS - 1);
}

static void test_a_real_safe_adds_its_clock_share(void)
{
    game_t g;
    begin(&g, 99);
    open_next(&g);
    CHECK(g.run_levels == 1);
    /* Opened at once: nearly the whole clock was left. */
    CHECK(g.run_speed > 9000 && g.run_speed <= 10000);
    CHECK(game_run_score(&g) > GAME_RUN_SAFE_POINTS);
    CHECK(game_run_score(&g) < 2 * GAME_RUN_SAFE_POINTS);
}

/* ---- Classic ends on the clock ------------------------------------------ */

static void test_classic_miss_keeps_the_clock_and_the_run(void)
{
    game_t g;
    begin(&g, 21);
    open_next(&g);
    s_now += GAME_REPLAY_LOCKOUT_MS;
    game_tap(&g, s_now);                               /* level 2 */
    s_now += 20000;
    const uint32_t left = game_time_left_ms(&g, s_now);
    const uint8_t old_first = g.code[0];
    go_to_target(&g);
    turn(&g, game_stage_dir(&g));                      /* a miss */
    CHECK(g.mode == GAME_WRONG && g.run_fails == 1 && !g.timed_out);
    CHECK(!game_run_over(&g));
    const uint32_t at_miss = game_time_left_ms(&g, s_now);   /* frozen in the red */
    const unsigned ev = hold_red(&g);
    CHECK((ev & GAME_EV_RESUMED) && (ev & GAME_EV_STARTED));
    CHECK(!(ev & GAME_EV_OVER));
    CHECK(g.mode == GAME_PLAY && g.level == 2 && g.run_levels == 1 && g.run_fails == 1);
    CHECK(g.ticks == 0 && g.stage == 0);
    (void)old_first;
    /* The red hold cost nothing: the clock is where it was. */
    CHECK(game_time_left_ms(&g, s_now) <= left);
    CHECK(game_time_left_ms(&g, s_now) == at_miss);          /* the red hold was not charged */
    CHECK(at_miss <= left);
    CHECK(game_time_left_ms(&g, s_now) < game_level_ms(2) - 15000);   /* not restarted to full */
}

static void test_classic_clock_running_out_ends_the_run(void)
{
    game_t g;
    begin(&g, 21);
    open_next(&g);
    s_now += GAME_REPLAY_LOCKOUT_MS;
    game_tap(&g, s_now);
    s_now += game_level_ms(2) + 1;
    CHECK(game_tick(&g, s_now) & GAME_EV_TIMEOUT);
    CHECK(g.run_fails == 0);                           /* the clock is not a fail */
    CHECK(game_run_over(&g));
    CHECK(g.run_levels == 1);
    const unsigned ev = hold_red(&g);
    CHECK(ev & GAME_EV_OVER);
    CHECK(g.mode == GAME_TITLE && g.rules == GAME_RULES_CLASSIC);
}

static void test_zen_never_ends(void)
{
    game_t g;
    begin_rules(&g, 8, GAME_RULES_ZEN);
    for (int i = 0; i < 5; i++) {
        lose(&g);
        CHECK(!game_run_over(&g));
        CHECK(!(hold_red(&g) & GAME_EV_OVER));
        CHECK(g.mode == GAME_PLAY);
    }
}

static void test_classic_loss_still_deals_fresh_numbers(void)
{
    game_t g;
    begin(&g, 5);
    lose(&g);
    const unsigned ev = hold_red(&g);
    CHECK(ev & GAME_EV_STARTED);
    CHECK(!(ev & GAME_EV_RETRY));
    CHECK(g.lives == 0);
}

static void test_zen_has_no_clock_and_no_score(void)
{
    game_t g;
    begin_rules(&g, 5, GAME_RULES_ZEN);
    CHECK(!game_timed(&g));
    s_now += game_level_ms(1) * 3;
    CHECK(game_tick(&g, s_now) == GAME_EV_NONE);
    CHECK(g.mode == GAME_PLAY);
    for (int i = 0; i < GAME_CODE_LEN; i++) {
        solve_stage(&g);
    }
    CHECK(g.mode == GAME_UNLOCKED);
    CHECK(game_run_score(&g) == 0);
}

static void test_rules_survive_reset_and_bad_values_fall_back(void)
{
    game_t g;
    game_init(&g, 5);
    game_set_rules(&g, GAME_RULES_ZEN);
    game_reset(&g, 2000);
    CHECK(g.rules == GAME_RULES_ZEN);
    game_set_rules(&g, (game_rules_t)99);
    CHECK(g.rules == GAME_RULES_CLASSIC);
}

/* The code must not depend on how many "felt?" rolls the turning used. One
 * game turns slowly (no rolls); the other crosses the middle speed band, where
 * every arrival rolls. Both reach the same dial positions, so they must be
 * dealt the same next code. */
static void test_codes_do_not_depend_on_the_feel_rolls(void)
{
    game_t a, b;
    begin(&a, 77);
    begin(&b, 77);
    for (int i = 0; i < GAME_CODE_LEN; i++) {
        CHECK(a.code[i] == b.code[i]);
    }
    uint32_t ta = s_now, tb = s_now;
    for (int stage = 0; stage < GAME_CODE_LEN; stage++) {
        const int dir = game_stage_dir(&a);
        for (int n = game_distance(&a); n > 0; n--) {
            ta += SLOW_MS;
            game_rotate(&a, dir, ta);
        }
        for (int n = game_distance(&b); n > 0; n--) {
            tb += 80;                  /* ~12 detents/s: inside the roll band */
            game_rotate(&b, dir, tb);
        }
        ta += SLOW_MS;
        tb += SLOW_MS;
        game_rotate(&a, -dir, ta);
        game_rotate(&b, -dir, tb);
    }
    CHECK(a.mode == GAME_UNLOCKED && b.mode == GAME_UNLOCKED);
    ta += GAME_REPLAY_LOCKOUT_MS;
    tb += GAME_REPLAY_LOCKOUT_MS;
    game_tap(&a, ta);
    game_tap(&b, tb);
    CHECK(a.ticks == b.ticks);
    for (int i = 0; i < GAME_CODE_LEN; i++) {
        CHECK(a.code[i] == b.code[i]);
    }
}

static void test_zen_miss_is_a_shorter_pause_than_other_modes(void)
{
    game_t g;
    begin_rules(&g, 5, GAME_RULES_ZEN);
    lose(&g);
    CHECK(g.mode == GAME_WRONG);
    CHECK(game_wrong_hold_ms(&g) == GAME_ZEN_HOLD_MS);
    CHECK(GAME_ZEN_HOLD_MS < GAME_WRONG_HOLD_MS);
    const uint32_t t = g.mode_since_ms;
    CHECK(game_tick(&g, t + GAME_ZEN_HOLD_MS - 1) == GAME_EV_NONE);
    CHECK(g.mode == GAME_WRONG);
    CHECK(game_tick(&g, t + GAME_ZEN_HOLD_MS) & GAME_EV_RESUMED);
    CHECK(g.mode == GAME_PLAY);
}

static void test_classic_and_lives_keep_the_full_hold(void)
{
    game_t c, l;
    begin_rules(&c, 5, GAME_RULES_CLASSIC);
    begin_rules(&l, 5, GAME_RULES_LIVES);
    CHECK(game_wrong_hold_ms(&c) == GAME_WRONG_HOLD_MS);
    CHECK(game_wrong_hold_ms(&l) == GAME_WRONG_HOLD_MS);
    lose(&c);
    CHECK(game_tick(&c, c.mode_since_ms + GAME_ZEN_HOLD_MS) == GAME_EV_NONE);
    CHECK(c.mode == GAME_WRONG);
}

static void test_lives_refill_when_the_levels_wrap_to_a_new_cycle(void)
{
    game_t g;
    begin_rules(&g, 5, GAME_RULES_LIVES);
    g.level = GAME_LEVELS;               /* as if levels 1-7 were cleared */
    lose(&g);
    hold_red(&g);                        /* a life gone, retrying level 8 */
    CHECK(g.lives == GAME_LIVES - 1);
    for (int i = 0; i < GAME_CODE_LEN; i++) {
        solve_stage(&g);
    }
    CHECK(g.mode == GAME_UNLOCKED);
    s_now += GAME_REPLAY_LOCKOUT_MS;
    game_tap(&g, s_now);
    CHECK(g.level == 1);                 /* a new cycle ... */
    CHECK(g.lives == GAME_LIVES);        /* ... starts with full lives */
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
    RUN(test_wrong_hold_then_new_code_same_level_same_clock);
    RUN(test_next_level_after_a_win_and_wraparound);
    RUN(test_tap_after_lockout_starts_next_level);
    RUN(test_game_new_goes_to_level_one);
    RUN(test_reset_returns_to_title);
    RUN(test_pause_stops_the_clock);
    RUN(test_pause_holds_the_red_timer);
    RUN(test_classic_is_the_default_and_has_no_lives);
    RUN(test_lives_start_full);
    RUN(test_lives_loss_keeps_the_same_code_and_costs_a_life);
    RUN(test_lives_retry_is_playable_and_far_enough_from_number_one);
    RUN(test_lives_red_hold_costs_no_time);
    RUN(test_lives_out_of_lives_ends_the_game);
    RUN(test_run_totals_add_up_across_safes_and_loops);
    RUN(test_run_totals_survive_lost_lives_and_end_with_the_game);
    RUN(test_run_score_more_safes_always_wins);
    RUN(test_run_score_same_safes_faster_wins);
    RUN(test_run_score_lives_fails_cost_speed_classic_fails_do_not);
    RUN(test_run_score_zero_without_safes_and_in_zen);
    RUN(test_a_real_safe_adds_its_clock_share);
    RUN(test_classic_miss_keeps_the_clock_and_the_run);
    RUN(test_classic_clock_running_out_ends_the_run);
    RUN(test_zen_never_ends);
    RUN(test_lives_a_win_keeps_the_lives_left);
    RUN(test_lives_timeout_ends_the_game_whatever_is_left);
    RUN(test_lives_new_game_restores_full_lives_and_level_one);
    RUN(test_classic_loss_still_deals_fresh_numbers);
    RUN(test_zen_has_no_clock_and_no_score);
    RUN(test_rules_survive_reset_and_bad_values_fall_back);
    RUN(test_codes_do_not_depend_on_the_feel_rolls);
    RUN(test_lives_refill_when_the_levels_wrap_to_a_new_cycle);
    RUN(test_zen_miss_is_a_shorter_pause_than_other_modes);
    RUN(test_classic_and_lives_keep_the_full_hold);

    printf("\n%d checks, %d failed\n", s_checks, s_failed);
    return s_failed ? 1 : 0;
}
