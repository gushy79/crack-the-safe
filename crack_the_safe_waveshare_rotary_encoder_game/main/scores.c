/*
 * Crack the Safe -- the high-score table. See scores.h.
 */

#include "scores.h"

#include <stdio.h>
#include <string.h>

void scores_clear(score_table_t *t)
{
    memset(t, 0, sizeof(*t));
}

bool scores_qualifies(const score_table_t *t, uint32_t score)
{
    if (score == 0) {
        return false;
    }
    return t->count < SCORES_MAX || score > t->e[SCORES_MAX - 1].score;
}

int scores_insert(score_table_t *t, score_entry_t entry)
{
    if (!scores_qualifies(t, entry.score)) {
        return -1;
    }
    /* First place whose score is strictly lower: an equal score goes below. */
    int rank = t->count;
    for (int i = 0; i < t->count; i++) {
        if (entry.score > t->e[i].score) {
            rank = i;
            break;
        }
    }
    int last = t->count < SCORES_MAX ? t->count : SCORES_MAX - 1;
    for (int i = last; i > rank; i--) {
        t->e[i] = t->e[i - 1];
    }
    t->e[rank] = entry;
    if (t->count < SCORES_MAX) {
        t->count++;
    }
    return rank;
}

bool scores_valid(const score_table_t *t)
{
    if (t->count > SCORES_MAX) {
        return false;
    }
    for (int i = 0; i < t->count; i++) {
        if (t->e[i].score == 0) {
            return false;
        }
        if (i > 0 && t->e[i].score > t->e[i - 1].score) {
            return false;
        }
    }
    return true;
}

void scores_fmt_thousands(uint32_t v, char *out, unsigned out_len)
{
    if (out_len == 0) {
        return;
    }
    char digits[16];
    const int n = snprintf(digits, sizeof(digits), "%lu", (unsigned long)v);
    unsigned o = 0;
    for (int i = 0; i < n && o + 1 < out_len; i++) {
        if (i > 0 && (n - i) % 3 == 0) {
            if (o + 2 >= out_len) {
                break;
            }
            out[o++] = ',';
        }
        out[o++] = digits[i];
    }
    out[o] = '\0';
}
