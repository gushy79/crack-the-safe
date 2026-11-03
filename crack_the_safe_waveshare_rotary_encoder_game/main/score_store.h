/*
 * The high-score tables in NVS, one blob per mode. The table logic is in
 * scores.c; this is only the flash.
 */
#pragma once

#include <stdbool.h>

#include "scores.h"

/** One table per scored mode (Zen has no score). */
#define SCORE_MODES 2   /* 0 Classic, 1 Lives */

/** Read a mode's table. A missing, wrong-sized or corrupt blob gives an empty
 *  table, never an error. */
void score_store_load(int mode, score_table_t *t);

/** Write a mode's table. Returns false (and logs) if flash refused it. */
bool score_store_save(int mode, const score_table_t *t);
