/*
 * Game glue: feeds the knob and taps into the game, turns its events into
 * haptics and screen changes, and drives the screen refresh.
 *
 * What each screen SHOWS is ui_game.c's job: text, states and the HIDDEN
 * flag, read from g_game. Nothing here touches a widget.
 */

#include "app.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "bt_link.h"
#include "knob_board.h"
#include "score_store.h"
#include "scores.h"
#include "screens.h"
#include "sound.h"
#include "ui.h"
#include "ui_game.h"

static const char *TAG = "safe";

game_t g_game;

/* DRV2605 ROM effects (datasheet table 11.2). */
#define FX_STRONG_CLICK  1     /* opening click */
#define FX_DOUBLE_CLICK  10    /* number locked in */
#define FX_SHARP_TICK    24    /* a game starts */
#define FX_HUM           64    /* the door swinging */
#define FX_RAMP_UP       82    /* the bolts drawing back */
#define FX_PAUSE(ms)     (0x80 | ((ms) / 10))

/* "Feeling" the number: a CONTINUOUS buzz that grows as the dial nears it.
 *
 *   more than feel_range() away  off
 *   feel_range() .. 1 away       held at FEEL_MIN .. FEEL_MAX, one step
 *                               stronger each detent closer
 *   on the number (felt)        FEEL_FULL, held until the player reverses
 *                               to lock it (double click) or goes past
 *   past it                     off -- that is the loss
 *
 * Levels are DRV2605 real-time drive, 0..127. Each detent re-arms the hold
 * for FEEL_HOLD_MS, so the buzz also stops if the knob is left alone. */
/* How far out the buzz starts: 10 detents up to level 4, then narrower each
 * level -- the higher levels are harder to feel, since their time limit no
 * longer halves. */
static int feel_range(void)
{
    static const uint8_t range[GAME_LEVELS] = { 10, 10, 10, 10, 8, 6, 5, 4 };
    const int lv = g_game.level ? g_game.level : 1;
    return range[(lv > GAME_LEVELS ? GAME_LEVELS : lv) - 1];
}
#define FEEL_MIN      30
#define FEEL_MAX      100
#define FEEL_FULL     127
#define FEEL_HOLD_MS  4000

static bool s_hum_played;
static bool s_prize_played;

/* High scores: one Top-5 table per scored mode (Classic, Lives; Zen is not
 * scored), kept in NVS. The last opened safe's score and rank are what the vault screen shows. */
static score_table_t s_scores[SCORE_MODES];
static uint32_t s_last_score;
static int s_last_rank = -1;       /* 0 = new best, -1 = not in the table */

/* The table for the current rules; Zen shows the Classic one (it records
 * nothing of its own). */
static int score_mode(void)
{
    return g_game.rules == GAME_RULES_LIVES ? 1 : 0;
}

/* The safe just opened: score it and, if it makes the table, save it. */
static void record_score(void)
{
    const uint32_t now = app_now_ms();
    s_last_score = game_current_score(&g_game, now);
    if (s_last_score == 0) {       /* Zen, or no time left: nothing to record */
        s_last_rank = -1;
        return;
    }
    const score_entry_t e = {
        .score  = s_last_score,
        .level  = g_game.level,
        .left_s = (uint16_t)(game_time_left_ms(&g_game, now) / 1000u),
    };
    const int m = score_mode();
    s_last_rank = scores_insert(&s_scores[m], e);
    if (s_last_rank >= 0) {
        score_store_save(m, &s_scores[m]);
    }
    ESP_LOGI(TAG, "score %lu, rank %d", (unsigned long)s_last_score, s_last_rank);
}

const score_table_t *app_scores(void)
{
    return &s_scores[score_mode()];
}

uint32_t app_last_score(void)
{
    return s_last_score;
}

int app_last_rank(void)
{
    return s_last_rank;
}

uint32_t app_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

uint32_t app_unlock_ms(void)
{
    return app_now_ms() - g_game.mode_since_ms;
}

int app_door_frame_at(uint32_t t)
{
    if (t < UNLOCK_DOOR_START_MS) {
        return 0;
    }
    const int f = 1 + (int)((t - UNLOCK_DOOR_START_MS) / UNLOCK_FRAME_MS);
    return f < UNLOCK_DOOR_FRAMES ? f : UNLOCK_DOOR_FRAMES - 1;
}

/* ---- Screens shown over the game -------------------------------------------
 *
 * The menu (touch and hold), Settings and Headphones are a small stack on top
 * of whatever the game is showing. While anything is on it the game is paused:
 * no turns, taps or ticks reach it and its clock is stopped. */

typedef enum { OVER_MENU, OVER_SETTINGS, OVER_HEADPHONES, OVER_SCORES } over_t;

#define OVER_MAX 3
static over_t s_over[OVER_MAX];
static int s_depth;

static bool over_is(over_t o)
{
    return s_depth && s_over[s_depth - 1] == o;
}

static void show_screen_for_mode(void)
{
    static int s_screen = SCREEN_ID_PAGE_TITLE;   /* ui_init() loads the first page */
    int want;
    if (s_depth) {
        switch (s_over[s_depth - 1]) {
        case OVER_MENU:     want = SCREEN_ID_PAGE_MENU;       break;
        case OVER_SETTINGS: want = SCREEN_ID_PAGE_SETTINGS;   break;
        case OVER_SCORES:   want = SCREEN_ID_PAGE_SCORES;     break;
        default:            want = SCREEN_ID_PAGE_HEADPHONES; break;
        }
    } else {
        switch (g_game.mode) {
        case GAME_TITLE:        want = SCREEN_ID_PAGE_TITLE;        break;
        case GAME_INSTRUCTIONS: want = SCREEN_ID_PAGE_INSTRUCTIONS; break;
        case GAME_UNLOCKED:     want = SCREEN_ID_PAGE_UNLOCKED;     break;
        default:                want = SCREEN_ID_PAGE_PLAY;         break;
        }
    }
    if (want != s_screen) {
        /* Paint the page for its first frame BEFORE it fades in, so the
         * vault never flashes its authored (door-open) state. */
        ui_game_refresh();
        loadScreen((enum ScreensEnum)want);
        s_screen = want;
    }
}

static void over_push(over_t o)
{
    if (s_depth == OVER_MAX) {
        return;
    }
    if (s_depth == 0) {
        game_pause(&g_game, app_now_ms());
        knob_board_haptic_pulse(0, 0);      /* stop a buzz still holding */
    }
    s_over[s_depth++] = o;
    if (o == OVER_HEADPHONES) {
        /* Start looking straight away, unless already busy. */
        bt_link_state_t s;
        bt_link_get(&s);
        if (s.present && s.conn == BT_CONN_IDLE && !s.searching) {
            bt_link_search();
        }
    }
    show_screen_for_mode();
}

static void over_pop(void)
{
    if (!s_depth) {
        return;
    }
    if (over_is(OVER_HEADPHONES)) {
        bt_link_state_t s;
        bt_link_get(&s);
        if (s.searching) {
            bt_link_stop_search();
        }
        ui_game_headphones_info(false);
    }
    if (--s_depth == 0) {
        game_resume(&g_game, app_now_ms());
    }
    show_screen_for_mode();
}

static void handle(unsigned ev)
{
    if (ev & GAME_EV_STARTED) {
        ESP_LOGI(TAG, "new code dealt");
        knob_board_haptic(FX_SHARP_TICK);
    }
    if (ev & GAME_EV_RETRY) {
        ESP_LOGI(TAG, "life lost, %u left -- same code again", g_game.lives);
        knob_board_haptic(FX_SHARP_TICK);
    }
    if (ev & GAME_EV_HIT) {
        sound_pin();
    }
    if (ev & GAME_EV_LOCKED) {
        knob_board_haptic(FX_DOUBLE_CLICK);
        ESP_LOGI(TAG, "number %u locked", g_game.locked);
    }
    if (ev & GAME_EV_WRONG) {
        /* Past the number, or out of time: the feeling stops dead (which
         * also cuts a pulse still running), the dial flashes police lights
         * and the siren wails. */
        knob_board_haptic_pulse(0, 0);
        sound_siren();       /* for as long as the police lights flash */
        ESP_LOGI(TAG, "wrong -- attempt %u, new code", g_game.attempts);
    }
    if (ev & GAME_EV_UNLOCKED) {
        static const uint8_t seq[] = { FX_STRONG_CLICK, FX_PAUSE(150), FX_RAMP_UP };
        knob_board_haptic_sequence(seq, sizeof(seq));
        sound_vault_door();
        s_hum_played = false;
        s_prize_played = false;
        record_score();
        ESP_LOGI(TAG, "unlocked in %u attempt(s), %lus", g_game.attempts,
                 (unsigned long)game_elapsed_s(&g_game, app_now_ms()));
    }
    if (ev) {
        show_screen_for_mode();
    }
}

/* Set the buzz for where the dial now is. */
static void feel(void)
{
    if (g_game.mode != GAME_PLAY) {
        return;
    }
    if (g_game.on_target) {
        /* Full when the hit was felt; silent when it came too fast to feel,
         * which is the cue a speeding player misses. */
        knob_board_haptic_pulse(g_game.hit_felt ? FEEL_FULL : 0, FEEL_HOLD_MS);
        return;
    }
    const int d = game_distance(&g_game);
    const int range = feel_range();
    if (d <= 0 || d > range) {
        knob_board_haptic_pulse(0, 0);     /* off: far away, or slipped past */
        return;
    }
    const int level = FEEL_MAX - (FEEL_MAX - FEEL_MIN) * (d - 1) / (range - 1);
    knob_board_haptic_pulse((uint8_t)level, FEEL_HOLD_MS);
}

void app_rotate(int detents)
{
    if (s_depth) {
        if (over_is(OVER_HEADPHONES)) {
            ui_game_scroll_headphones(detents);
        } else if (over_is(OVER_SETTINGS)) {
            app_volume_step(detents);        /* the knob turns the volume */
        }
        return;                              /* the game is paused */
    }
    const unsigned ev = game_rotate(&g_game, detents, app_now_ms());
    handle(ev);
    /* A lock plays its double click instead; a loss already cut the buzz. */
    if (!(ev & (GAME_EV_LOCKED | GAME_EV_WRONG | GAME_EV_UNLOCKED))) {
        feel();
    }
    ui_game_refresh();
}

void app_tap(void)
{
    if (s_depth) {
        return;
    }
    handle(game_tap(&g_game, app_now_ms()));
    ui_game_refresh();
}

void app_start(void)
{
    handle(game_start(&g_game, app_now_ms()));
    ui_game_refresh();
}

/* ---- Menu --------------------------------------------------------------- */

void app_menu(void)
{
    if (s_depth == 0) {
        knob_board_haptic(FX_DOUBLE_CLICK);
        over_push(OVER_MENU);
        ui_game_refresh();
    }
}

void app_new_game(game_rules_t rules)
{
    s_depth = 0;                            /* the paused game is dropped */
    sound_stop();                           /* a siren still wailing */
    game_set_rules(&g_game, rules);
    const unsigned ev = game_new(&g_game, app_now_ms());
    ESP_LOGI(TAG, "new game, rules %d", (int)g_game.rules);
    handle(ev);
    ui_game_refresh();
}

void app_menu_new_game(void)
{
    app_new_game(g_game.rules);             /* the Modes page comes in Step 4 */
}

void app_menu_settings(void)
{
    over_push(OVER_SETTINGS);
    ui_game_refresh();
}

void app_menu_scores(void)
{
    over_push(OVER_SCORES);
    ui_game_refresh();
}

void app_back(void)
{
    over_pop();
    ui_game_refresh();
}

void app_volume_step(int steps)
{
    sound_set_volume(sound_volume() + steps);
    ui_game_refresh();
}

/* ---- Headphones ------------------------------------------------------------- */

void app_headphones_open(void)
{
    over_push(OVER_HEADPHONES);
    ui_game_refresh();
}

void app_headphones_search(void)
{
    bt_link_search();
}

void app_headphones_select(int row)
{
    bt_link_state_t s;
    bt_dev_t d;
    bt_link_get(&s);
    if (!bt_link_row(&s, row, &d)) {
        return;
    }
    if (row == 0 && s.conn != BT_CONN_IDLE) {
        return;                  /* already connected, or on its way */
    }
    knob_board_haptic(FX_SHARP_TICK);
    bt_link_connect(d.addr);
}

void app_headphones_info(bool open)
{
    ui_game_headphones_info(open);
    ui_game_refresh();
}

void app_headphones_forget(void)
{
    knob_board_haptic(FX_DOUBLE_CLICK);
    bt_link_forget();
    ui_game_headphones_info(false);
    ui_game_refresh();
}

/* 20 ms, in the LVGL task: the red hold, the unlock timeline, the refresh. */
static void app_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (s_depth) {
        ui_game_refresh();                  /* paused: only the menus move */
        return;
    }
    handle(game_tick(&g_game, app_now_ms()));
    if (g_game.mode == GAME_UNLOCKED && !s_hum_played &&
        app_unlock_ms() >= UNLOCK_DOOR_START_MS) {
        knob_board_haptic(FX_HUM);
        s_hum_played = true;
    }
    if (g_game.mode == GAME_UNLOCKED && !s_prize_played &&
        app_unlock_ms() >= UNLOCK_REWARD_MS) {
        sound_prize(g_game.level);
        s_prize_played = true;
    }
    ui_game_refresh();
}

void app_init(uint32_t seed)
{
    bt_link_init();
    sound_init();
    for (int m = 0; m < SCORE_MODES; m++) {
        score_store_load(m, &s_scores[m]);
    }
    game_init(&g_game, seed);
    ui_game_refresh();
    lv_timer_create(app_timer_cb, 20, NULL);
}
