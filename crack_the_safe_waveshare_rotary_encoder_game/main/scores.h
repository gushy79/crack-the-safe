/*
 * Crack the Safe -- the high-score table, with no hardware in it.
 *
 * A table keeps the best SCORES_MAX scores, best first. Saving it (NVS) and
 * showing it (the Scores page) are the caller's job; this file only decides
 * who gets in and where, and whether a table read back from flash can be
 * trusted.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define SCORES_MAX 5
/* The longest clock is 5:00, so no entry can have more seconds left. */
#define SCORES_MAX_LEFT_S 300
/* A run longer than this many safes is not something a table will hold. */
#define SCORES_MAX_LEVELS 9999

/* Classic: one safe -- its level (1..GAME_LEVELS) and the seconds left.
 * Lives: a whole run, every safe opened until the lives or the clock ran
 * out -- the sum of their scores and how many (GAME_LEVELS make one loop of
 * the levels); left_s is 0. */
typedef struct {
    uint32_t score;
    uint16_t levels;    /* 1..SCORES_MAX_LEVELS */
    uint16_t left_s;    /* whole seconds that were left on the clock */
} score_entry_t;

typedef struct {
    uint8_t       count;            /* used entries, 0..SCORES_MAX */
    score_entry_t e[SCORES_MAX];    /* best first */
} score_table_t;

/** Empty the table. */
void scores_clear(score_table_t *t);

/** Would this score make the table? (Zero never does.) */
bool scores_qualifies(const score_table_t *t, uint32_t score);

/**
 * Add an entry. Returns its rank (0 = best) or -1 if it did not make the
 * table. An equal score ranks below the ones already there, so the earlier
 * score keeps its place.
 */
int scores_insert(score_table_t *t, score_entry_t entry);

/** Is a table read back from storage consistent (count in range, best
 *  first, no empty scores)? A bad one should be replaced by an empty one. */
bool scores_valid(const score_table_t *t);

/** A run for a table row or the game-over line: "12,345 \xC2\xB7 19 lvls \xC2\xB7 2 loops"
 *  (loops only once a whole loop of the levels was completed). */
void scores_fmt_run(score_entry_t e, char *out, unsigned out_len);

/** Write a number with thousands separators ("1,920") into out, always
 *  NUL-terminated and truncated to fit. */
void scores_fmt_thousands(uint32_t v, char *out, unsigned out_len);
