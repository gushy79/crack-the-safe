/*
 * Player settings kept in NVS: the haptic strength and the chosen game rules.
 * (The volume is saved by sound.c.) haptic_scale() is pure, so it is tested on
 * the host; the rest needs NVS.
 */
#pragma once

#include <stdint.h>

#define HAPTIC_MIN     1
#define HAPTIC_MAX     5
#define HAPTIC_DEFAULT HAPTIC_MAX

/** Scale a DRV2605 RTP level (0..127) by a strength of 1..5, which is 40, 55,
 *  70, 85 and 100 percent. Silence stays silent and any buzz stays a buzz. */
static inline uint8_t haptic_scale(uint8_t level, int strength)
{
    if (strength < HAPTIC_MIN) {
        strength = HAPTIC_MIN;
    }
    if (strength > HAPTIC_MAX) {
        strength = HAPTIC_MAX;
    }
    if (level == 0) {
        return 0;
    }
    const unsigned pct = 25u + 15u * (unsigned)strength;
    const unsigned v = (level * pct + 50u) / 100u;
    return (uint8_t)(v ? v : 1u);
}

/** Load both settings from NVS (call once at start). Anything missing or out
 *  of range falls back to the default. */
void settings_init(void);

int  settings_haptic(void);
/** Clamped to HAPTIC_MIN..HAPTIC_MAX and saved at once. */
void settings_set_haptic(int strength);

/** The chosen game_rules_t, as an int. */
int  settings_rules(void);
void settings_set_rules(int rules);
