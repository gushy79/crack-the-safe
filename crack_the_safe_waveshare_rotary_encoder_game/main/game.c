/*
 * Crack the Safe -- rules. See game.h for the rules in prose.
 */

#include "game.h"

#include <stdlib.h>

/* Consecutive numbers (and number 1 vs the dial's resting position) are at
 * least this far apart, so every number takes a real turn to reach. */
#define MIN_GAP 5

static uint32_t rnd(uint32_t *s)
{
    /* xorshift32 -- the seed comes from the hardware RNG in main.c. */
    *s ^= *s << 13;
    *s ^= *s >> 17;
    *s ^= *s << 5;
    return *s;
}

static int wrap(int v)
{
    v %= GAME_POSITIONS;
    return v < 0 ? v + GAME_POSITIONS : v;
}

static int dist(int a, int b)
{
    const int d = abs(a - b) % GAME_POSITIONS;
    return d < GAME_POSITIONS - d ? d : GAME_POSITIONS - d;
}

int game_dial_value(const game_t *g)
{
    /* Turning right moves the ring clockwise, so the number under the index
     * counts DOWN -- the way a real combination dial reads. */
    return wrap(-g->ticks);
}

int game_stage_dir(const game_t *g)
{
    return (g->stage % 2 == 0) ? +1 : -1;
}

int game_distance(const game_t *g)
{
    if (g->mode != GAME_PLAY || g->stage >= GAME_CODE_LEN) {
        return -1;
    }
    const int v = game_dial_value(g), target = g->code[g->stage];
    /* Turning right counts the number under the index DOWN. */
    return game_stage_dir(g) > 0 ? wrap(v - target) : wrap(target - v);
}

static void set_mode(game_t *g, game_mode_t m, uint32_t now_ms)
{
    g->mode = m;
    g->mode_since_ms = now_ms;
}

/* Deal a new code and go back to number 1. */
static void deal(game_t *g, uint32_t now_ms)
{
    int prev = game_dial_value(g);
    g->start_dial = (uint8_t)prev;
    for (int k = 0; k < GAME_CODE_LEN; k++) {
        int v;
        do {
            v = (int)(rnd(&g->rng_deal) % GAME_POSITIONS);
        } while (dist(v, prev) < MIN_GAP);
        g->code[k] = (uint8_t)v;
        prev = v;
    }
    g->stage = 0;
    g->locked = 0;
    g->travel = 0;
    g->on_target = false;
    g->hit_felt = false;
    (void)now_ms;
}

uint32_t game_level_ms(int level)
{
    static const uint32_t limit[GAME_LEVELS] = {
        300000u, 150000u, 60000u, 30000u,      /* halving ...                   */
        20000u, 20000u, 20000u, 20000u,        /* ... down to a 20 s floor      */
    };
    if (level < 1) {
        level = 1;
    }
    if (level > GAME_LEVELS) {
        level = GAME_LEVELS;
    }
    return limit[level - 1];
}

int game_next_level(const game_t *g)
{
    if (g->mode == GAME_UNLOCKED) {
        return g->level >= GAME_LEVELS ? 1 : g->level + 1;
    }
    return g->level ? g->level : 1;
}

uint32_t game_time_left_ms(const game_t *g, uint32_t now_ms)
{
    if (g->mode == GAME_TITLE || g->mode == GAME_INSTRUCTIONS) {
        return game_level_ms(1);
    }
    const uint32_t end = g->finished_ms ? g->finished_ms : now_ms;
    const uint32_t used = end - g->started_ms;
    const uint32_t limit = game_level_ms(g->level);
    return used >= limit ? 0 : limit - used;
}

static unsigned start_round(game_t *g, uint32_t now_ms)
{
    if (g->mode != GAME_UNLOCKED) {
        /* Not carrying on from a win: a new run (a win continues it, round
         * the levels as many times as the player lasts). */
        g->run_score = 0;
        g->run_levels = 0;
    }
    g->level = (uint8_t)game_next_level(g);
    /* A fresh game starts with full lives; a win carries what is left. */
    if (g->rules != GAME_RULES_LIVES) {
        g->lives = 0;
    } else if (g->mode != GAME_UNLOCKED || g->level == 1) {
        /* A fresh game, or the levels wrapping round to a new cycle. */
        g->lives = GAME_LIVES;
    }
    if (g->mode == GAME_UNLOCKED && g->level == 1) {
        g->loops++;                /* a win on the last level: round again */
    }
    g->timed_out = false;
    g->attempts = 1;
    g->started_ms = now_ms;
    g->finished_ms = 0;
    deal(g, now_ms);
    set_mode(g, GAME_PLAY, now_ms);
    return GAME_EV_STARTED;
}

void game_init(game_t *g, uint32_t seed)
{
    *g = (game_t){ 0 };
    g->rng_deal = seed ? seed : 0x9E3779B9u;
    g->rng_feel = (g->rng_deal * 2654435761u) ^ 0x85EBCA6Bu;
    if (g->rng_feel == 0) {
        g->rng_feel = 0x27D4EB2Fu;
    }
    g->mode = GAME_TITLE;
}

/* Game over. The red hold shows what went wrong; when it ends, game_tick()
 * starts a fresh game with the dial back at 0. */
static unsigned fail(game_t *g, uint32_t now_ms)
{
    g->on_target = false;
    g->hit_felt = false;
    g->finished_ms = now_ms;       /* freezes the clock display in the red */
    if (g->rules == GAME_RULES_LIVES && g->lives > 0) {
        g->lives--;
    }
    set_mode(g, GAME_WRONG, now_ms);
    return GAME_EV_WRONG;
}

static unsigned open_safe(game_t *g, uint32_t now_ms)
{
    g->on_target = false;
    g->hit_felt = false;
    g->locked = GAME_CODE_LEN;
    g->stage = GAME_CODE_LEN;
    g->finished_ms = now_ms;
    set_mode(g, GAME_UNLOCKED, now_ms);
    g->run_score += game_current_score(g, now_ms);
    g->run_levels++;
    return GAME_EV_UNLOCKED;
}

/* Is the click on the number felt at the current turning speed? */
static bool felt(game_t *g)
{
    if (g->speed_dps <= GAME_FEEL_SLOW_DPS) {
        return true;
    }
    if (g->speed_dps >= GAME_FEEL_FAST_DPS) {
        return false;
    }
    const uint32_t span = GAME_FEEL_FAST_DPS - GAME_FEEL_SLOW_DPS;
    const uint32_t keep = GAME_FEEL_FAST_DPS - g->speed_dps;   /* span..0 */
    return rnd(&g->rng_feel) % span < keep;
}

/* Smoothed detents per second. Detents arrive in batches (the board delivers
 * every 10 ms), so speed is measured batch to batch; after a pause it starts
 * over rather than averaging in the idle time. */
static void update_speed(game_t *g, int detents, uint32_t now_ms)
{
    const uint32_t n = (uint32_t)abs(detents);
    const uint32_t dt = now_ms - g->last_turn_ms;
    g->last_turn_ms = now_ms;
    if (dt >= 500) {
        g->speed_dps = 0;          /* first detent after a pause: slow */
        return;
    }
    const uint32_t inst = n * 1000u / (dt ? dt : 1u);
    g->speed_dps = (g->speed_dps + inst) / 2;
}

/* One detent of play. */
static unsigned step(game_t *g, int delta, uint32_t now_ms)
{
    g->ticks += delta;
    const int v = game_dial_value(g);
    const int dir = game_stage_dir(g);

    if (delta == dir) {
        if (g->on_target) {
            /* Kept going past the number instead of reversing on it. */
            g->timed_out = false;
            return fail(g, now_ms);
        }
        if (g->travel < 255) {
            g->travel++;
        }
        if (v == g->code[g->stage]) {
            /* On the number, whether or not it is felt: going past it now
             * loses, and stopping on it counts (reverse to lock; the last
             * number also opens after a dwell, in game_tick). Speed only
             * decides whether the player gets the HIT that tells them. */
            g->on_target = true;
            g->hit_felt = felt(g);
            return g->hit_felt ? GAME_EV_HIT : GAME_EV_NONE;
        }
        return GAME_EV_NONE;
    }

    /* Reversing. */
    if (g->on_target && g->stage == GAME_CODE_LEN - 1) {
        return open_safe(g, now_ms);
    }
    if (g->on_target) {
        g->locked++;
        g->stage++;
        g->travel = 1;            /* this detent already counts toward the next */
        g->on_target = false;
        g->hit_felt = false;
        return GAME_EV_LOCKED;
    }
    if (g->travel >= 2) {
        /* Committed to a direction, then turned back somewhere other than
         * the number. */
        return fail(g, now_ms);
    }
    /* A detent or two of wobble before committing is forgiven. */
    if (g->travel > 0) {
        g->travel--;
    }
    return GAME_EV_NONE;
}

unsigned game_rotate(game_t *g, int detents, uint32_t now_ms)
{
    unsigned ev = GAME_EV_NONE;
    const int sign = detents > 0 ? +1 : -1;

    switch (g->mode) {
    case GAME_TITLE:
        /* The dial does not move: it is still at 0 when Start is touched. */
        set_mode(g, GAME_INSTRUCTIONS, now_ms);
        return GAME_EV_INSTRUCTIONS;

    case GAME_INSTRUCTIONS:
        return GAME_EV_NONE;      /* only the Start button moves on */

    case GAME_UNLOCKED:
        if (now_ms - g->mode_since_ms >= GAME_REPLAY_LOCKOUT_MS) {
            return start_round(g, now_ms);
        }
        return GAME_EV_NONE;

    case GAME_WRONG:
        return GAME_EV_NONE;      /* frozen in red until the reset */

    case GAME_PLAY:
        update_speed(g, detents, now_ms);
        /* Detent by detent: a fast spin is judged at every position it
         * crosses, so passing the number is never skipped over. */
        for (int i = 0; i != detents; i += sign) {
            ev |= step(g, sign, now_ms);
            if (g->mode != GAME_PLAY) {
                break;            /* rest of the spin is discarded */
            }
        }
        return ev;
    }
    return ev;
}

unsigned game_tap(game_t *g, uint32_t now_ms)
{
    if (g->mode == GAME_TITLE) {
        set_mode(g, GAME_INSTRUCTIONS, now_ms);
        return GAME_EV_INSTRUCTIONS;
    }
    if (g->mode == GAME_UNLOCKED &&
        now_ms - g->mode_since_ms >= GAME_REPLAY_LOCKOUT_MS) {
        return start_round(g, now_ms);
    }
    return GAME_EV_NONE;
}

unsigned game_start(game_t *g, uint32_t now_ms)
{
    if (g->mode == GAME_INSTRUCTIONS) {
        return start_round(g, now_ms);
    }
    return GAME_EV_NONE;
}

void game_set_rules(game_t *g, game_rules_t rules)
{
    g->rules = rules < GAME_RULES_COUNT ? rules : GAME_RULES_CLASSIC;
}

uint32_t game_wrong_hold_ms(const game_t *g)
{
    return g->rules == GAME_RULES_ZEN ? GAME_ZEN_HOLD_MS : GAME_WRONG_HOLD_MS;
}

bool game_timed(const game_t *g)
{
    return g->rules != GAME_RULES_ZEN;
}

unsigned game_new(game_t *g, uint32_t now_ms)
{
    return game_reset(g, now_ms) | start_round(g, now_ms);
}

void game_pause(game_t *g, uint32_t now_ms)
{
    if (!g->paused) {
        g->paused = true;
        g->paused_ms = now_ms;
    }
}

void game_resume(game_t *g, uint32_t now_ms)
{
    if (!g->paused) {
        return;
    }
    const uint32_t d = now_ms - g->paused_ms;
    g->paused = false;
    /* Shift every timestamp the rules measure from by the time away. */
    g->mode_since_ms += d;
    g->last_turn_ms += d;
    if (g->started_ms) {
        g->started_ms += d;
    }
    if (g->finished_ms) {
        g->finished_ms += d;
    }
}

unsigned game_reset(game_t *g, uint32_t now_ms)
{
    const uint32_t keep_deal = g->rng_deal, keep_feel = g->rng_feel;
    const game_rules_t keep_rules = g->rules;
    game_init(g, keep_deal);          /* title, level 0 (so 1 next), ticks 0 */
    g->rng_deal = keep_deal;
    g->rng_feel = keep_feel;
    g->rules = keep_rules;
    set_mode(g, GAME_TITLE, now_ms);
    return GAME_EV_RESET;
}

unsigned game_tick(game_t *g, uint32_t now_ms)
{
    if (g->mode == GAME_PLAY && g->on_target && g->stage == GAME_CODE_LEN - 1 &&
        now_ms - g->last_turn_ms >= GAME_OPEN_DWELL_MS) {
        return open_safe(g, now_ms);
    }
    if (g->mode == GAME_PLAY && game_timed(g) && game_time_left_ms(g, now_ms) == 0) {
        const unsigned ev = fail(g, now_ms);
        g->timed_out = true;
        g->lives = 0;              /* a spent clock cannot be retried */
        return ev | GAME_EV_TIMEOUT;
    }
    if (g->mode == GAME_WRONG && g->rules == GAME_RULES_LIVES && g->lives > 0 &&
        now_ms - g->mode_since_ms >= game_wrong_hold_ms(g)) {
        /* A life lost, not the game: the same code again from number 1, the
         * dial back where the code was dealt from. The clock kept its value
         * through the red hold, so the hold costs no time. */
        g->ticks = -(int32_t)g->start_dial;
        g->speed_dps = 0;
        g->stage = 0;
        g->locked = 0;
        g->travel = 0;
        g->on_target = false;
        g->hit_felt = false;
        g->timed_out = false;
        g->attempts++;
        g->started_ms += now_ms - g->finished_ms;
        g->finished_ms = 0;
        set_mode(g, GAME_PLAY, now_ms);
        return GAME_EV_RESUMED | GAME_EV_RETRY;
    }
    if (g->mode == GAME_WRONG && g->rules == GAME_RULES_LIVES &&
        now_ms - g->mode_since_ms >= game_wrong_hold_ms(g)) {
        /* The run is over (the last life, or the clock): back
         * to the title. The caller has the run's totals from the loss. */
        return game_reset(g, now_ms) | GAME_EV_OVER;
    }
    if (g->mode == GAME_WRONG && now_ms - g->mode_since_ms >= game_wrong_hold_ms(g)) {
        /* Classic and Zen: a new game: dial back to 0, fresh numbers dealt from there, the
         * attempt count and the clock restarted. */
        g->ticks = 0;
        g->speed_dps = 0;
        return start_round(g, now_ms) | GAME_EV_RESUMED;
    }
    return GAME_EV_NONE;
}

uint32_t game_score(int level, uint32_t time_left_ms)
{
    if (level < 1) {
        level = 1;
    }
    if (level > GAME_LEVELS) {
        level = GAME_LEVELS;
    }
    return (uint32_t)level * (time_left_ms / 100u);
}

uint32_t game_current_score(const game_t *g, uint32_t now_ms)
{
    if (g->mode != GAME_UNLOCKED || g->rules == GAME_RULES_ZEN) {
        return 0;
    }
    return game_score(g->level, game_time_left_ms(g, now_ms));
}

uint32_t game_elapsed_s(const game_t *g, uint32_t now_ms)
{
    if (g->started_ms == 0 && (g->mode == GAME_TITLE || g->mode == GAME_INSTRUCTIONS)) {
        return 0;
    }
    const uint32_t end = g->finished_ms ? g->finished_ms : now_ms;
    return (end - g->started_ms) / 1000u;
}
