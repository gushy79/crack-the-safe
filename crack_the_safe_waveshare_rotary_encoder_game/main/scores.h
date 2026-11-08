/*
 * Crack the Safe -- the high-score table, with no hardware in it.
 *
 * A table keeps the best SCORES_MAX scores, best first (the Scores page
 * shows the top SCORES_SHOWN; the rest are what lets a new score say "you
 * came 104th"). Saving it (NVS) and
 * showing it (the Scores page) are the caller's job; this file only decides
 * who gets in and where, and whether a table read back from flash can be
 * trusted.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* How many places the Scores page lists, and how many the table remembers. */
#define SCORES_SHOWN 5
#define SCORES_MAX 250
#define SCORES_MAX_LEVELS 9999

/* One run (see game_run_score): its score, the safes opened (GAME_LEVELS
 * make one loop of the levels) and the misses. The score is
 * levels x 10,000 + speed points, so it always agrees with `levels`. */
typedef struct {
    uint32_t score;
    uint32_t when;      /* local date and time as seconds since 1970-01-01 00:00
                           (UTC plus the phone's offset at setup); 0 = unknown,
                           the clock was not set (see wifi_link.h) */
    uint16_t levels;    /* 1..SCORES_MAX_LEVELS */
    uint16_t fails;
} score_entry_t;

typedef struct {
    uint16_t      count;            /* used entries, 0..SCORES_MAX */
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

/** A run for a table row or the game-over line:
 *  "192,345 \xC2\xB7 19 lvls \xC2\xB7 2 loops \xC2\xB7 3 fails" (loops once a whole loop
 *  of the levels was completed, fails when there were any and `show_fails`:
 *  Lives leaves them out, a lost life is already in its score). */
void scores_fmt_run(score_entry_t e, bool show_fails, char *out, unsigned out_len);

/** A table saved before runs had a date (entries of score, levels, fails only,
 *  8 bytes each after a 4-byte header). Converts it, dates unknown; false if
 *  the bytes are not such a table. */
bool scores_from_v1(const void *blob, unsigned len, score_table_t *out);

/** "Fri 7 Nov 2026, 21:45" from a `when`; "Date not recorded" for 0. */
void scores_fmt_when(uint32_t when, char *out, unsigned out_len);

/** Write a number with thousands separators ("1,920") into out, always
 *  NUL-terminated and truncated to fit. */
void scores_fmt_thousands(uint32_t v, char *out, unsigned out_len);
