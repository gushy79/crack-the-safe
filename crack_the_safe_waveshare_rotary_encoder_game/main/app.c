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
#include "settings.h"
#include "sound.h"
#include "ui.h"
#include "ui_game.h"
#include "wifi_link.h"

static const char *TAG = "safe";

game_t g_game;

/* DRV2605 ROM effects (datasheet table 11.2). */
#define FX_STRONG_CLICK  1     /* opening click */
#define FX_DOUBLE_CLICK  10    /* number locked in */
#define FX_SHARP_TICK    24    /* a game starts */
#define FX_SOFT_BUMP     8     /* a miss in Zen: gentle */
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

/* High scores: one Top-5 table per scored mode, kept in NVS. Classic's
 * entries are single safes; Lives' are whole runs (every safe opened until
 * the lives or the clock ran out). Zen is not scored. */
static score_table_t s_scores[SCORE_MODES];
/* The latest entry made: its table and place (-1 = it did not make the
 * table), shown highlighted on the Scores page. A run that did not place is
 * kept here to show on the game-over page. */
static int s_new_mode = -1;
static int s_new_rank = -1;
static score_entry_t s_new_entry;
static bool s_game_over;           /* the Scores page is showing a game over */
/* Which table the Scores page shows: 0 Classic, 1 Lives. It starts on the
 * current mode's and the knob flips it. */
static int s_scores_view;
static int s_save_mode = -1;       /* a table waiting to be saved, or -1 */

/* The table for the current rules; Zen shows the Classic one (it records
 * nothing of its own). */
static int score_mode(void)
{
    return g_game.rules == GAME_RULES_LIVES ? 1 : 0;
}

static void note_entry(int mode, score_entry_t e)
{
    s_new_mode = mode;
    s_new_entry = e;
    s_new_rank = scores_insert(&s_scores[mode], e);
    if (s_new_rank >= 0) {
        /* Saved a little later (flush_scores): the flash commit stalls the
         * screen, and this may be the moment the door animation starts. */
        s_save_mode = mode;
    }
}

/* When a run ended, as local seconds since 1970 (0 until the clock has been set). */
static uint32_t run_when(void)
{
    const uint32_t utc = wifi_link_unix_time();
    return utc ? (uint32_t)((int64_t)utc + (int64_t)wifi_link_tz_minutes() * 60) : 0;
}

/* The run is over (or given up): score it and, if it makes the table, save
 * it. Zen runs are not scored. */
static void record_run(void)
{
    if (g_game.rules == GAME_RULES_ZEN || g_game.run_levels == 0) {
        return;
    }
    note_entry(score_mode(), (score_entry_t){
        .score  = game_run_score(&g_game),
        .when   = run_when(),
        .levels = g_game.run_levels,
        .fails  = g_game.run_fails,
    });
    ESP_LOGI(TAG, "run %lu over %u safes, %u fails, rank %d",
             (unsigned long)game_run_score(&g_game), (unsigned)g_game.run_levels,
             (unsigned)g_game.run_fails, s_new_rank);
    g_game.run_speed = 0;          /* recorded: not again */
    g_game.run_levels = 0;
    g_game.run_fails = 0;
}

/* Write a waiting table to flash; a failure is logged, and the table stays in
 * memory (it is only lost if the power goes before the next win). */
static void flush_scores(void)
{
    if (s_save_mode >= 0) {
        score_store_save(s_save_mode, &s_scores[s_save_mode]);
        s_save_mode = -1;
    }
}

const score_table_t *app_scores(void)
{
    return &s_scores[s_scores_view];
}

int app_scores_view(void)
{
    return s_scores_view;
}

bool app_scores_game_over(void)
{
    return s_game_over;
}

bool app_scores_latest(score_entry_t *e, int *rank)
{
    if (s_new_mode != s_scores_view) {
        return false;
    }
    *e = s_new_entry;
    *rank = s_new_rank;
    return true;
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

typedef enum { OVER_MENU, OVER_SETTINGS, OVER_HEADPHONES, OVER_SCORES, OVER_MODES, OVER_WIFI } over_t;

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
        case OVER_MODES:    want = SCREEN_ID_PAGE_MODES;      break;
        case OVER_WIFI:     want = SCREEN_ID_PAGE_WIFI;       break;
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
    if (over_is(OVER_SCORES)) {
        ui_game_score_detail(-1);
    }
    if (over_is(OVER_WIFI)) {
        wifi_link_provision_stop();         /* hotspot off; joins the network if one was chosen */
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
        if (g_game.rules == GAME_RULES_ZEN) {
            /* Practice: a soft bump and a gentle chime, no police. */
            knob_board_haptic(FX_SOFT_BUMP);
            sound_soft_miss();
        } else {
            sound_siren();   /* for as long as the police lights flash */
        }
        ESP_LOGI(TAG, "wrong -- attempt %u, new code", g_game.attempts);
        if (game_run_over(&g_game)) {
            record_run();           /* the last life, or the clock: the run is over */
        }
    }
    if (ev & GAME_EV_UNLOCKED) {
        static const uint8_t seq[] = { FX_STRONG_CLICK, FX_PAUSE(150), FX_RAMP_UP };
        knob_board_haptic_sequence(seq, sizeof(seq));
        sound_vault_door();
        s_hum_played = false;
        s_prize_played = false;
        ESP_LOGI(TAG, "unlocked in %u attempt(s), %lus", g_game.attempts,
                 (unsigned long)game_elapsed_s(&g_game, app_now_ms()));
    }
    if (ev & GAME_EV_OVER) {
        /* The red hold of the run's last loss is over: the Scores page shows
         * how it went, and its button plays again. */
        ESP_LOGI(TAG, "game over");
        s_game_over = true;
        s_scores_view = score_mode();
        over_push(OVER_SCORES);
    }
    if (ev) {
        show_screen_for_mode();
    }
}

static void buzz(int level, uint32_t hold_ms);

/* Set the buzz for where the dial now is. */
static void feel(void)
{
    if (g_game.mode != GAME_PLAY) {
        return;
    }
    if (g_game.on_target) {
        /* Full when the hit was felt; silent when it came too fast to feel,
         * which is the cue a speeding player misses. */
        buzz(g_game.hit_felt ? FEEL_FULL : 0, FEEL_HOLD_MS);
        return;
    }
    const int d = game_distance(&g_game);
    const int range = feel_range();
    if (d <= 0 || d > range) {
        knob_board_haptic_pulse(0, 0);     /* off: far away, or slipped past */
        return;
    }
    const int level = FEEL_MAX - (FEEL_MAX - FEEL_MIN) * (d - 1) / (range - 1);
    buzz(level, FEEL_HOLD_MS);
}

void app_rotate(int detents)
{
    if (s_depth) {
        if (over_is(OVER_HEADPHONES)) {
            ui_game_scroll_headphones(detents);
        } else if (over_is(OVER_SETTINGS)) {
            app_volume_step(detents);        /* the knob turns the volume */
        } else if (over_is(OVER_SCORES) && !s_game_over) {
            ui_game_score_detail(-1);
            s_scores_view = (s_scores_view + 1) % SCORE_MODES;   /* the other table */
            ui_game_refresh();
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
        /* The finger is still down on a screen that is about to change: its
         * release must not press whatever button ends up beneath it. */
        lv_indev_t *const indev = lv_indev_active();
        if (indev) {
            lv_indev_wait_release(indev);
        }
        over_push(OVER_MENU);
        ui_game_refresh();
    }
}

void app_new_game(game_rules_t rules)
{
    s_depth = 0;                            /* the paused game is dropped */
    s_game_over = false;
    record_run();                           /* a run given up still counts */
    sound_stop();                           /* a siren still wailing */
    game_set_rules(&g_game, rules);
    const unsigned ev = game_new(&g_game, app_now_ms());
    ESP_LOGI(TAG, "new game, rules %d", (int)g_game.rules);
    handle(ev);
    ui_game_refresh();
}

/* New Game from the menu opens the Modes page; picking a mode starts it. */
void app_menu_new_game(void)
{
    over_push(OVER_MODES);
    ui_game_refresh();
}

void app_modes_pick(game_rules_t rules)
{
    settings_set_rules((int)rules);         /* remembered for next time */
    app_new_game(rules);
}

void app_menu_settings(void)
{
    over_push(OVER_SETTINGS);
    ui_game_refresh();
}

void app_scores_row(int row)
{
    ui_game_score_detail(row);
    ui_game_refresh();
}

void app_settings_wifi(void)
{
    wifi_link_provision_start();            /* the hotspot and its page; the screen shows the QR */
    over_push(OVER_WIFI);
    ui_game_refresh();
}

void app_menu_scores(void)
{
    s_game_over = false;
    s_scores_view = score_mode();
    over_push(OVER_SCORES);
    ui_game_refresh();
}

void app_back(void)
{
    if (s_game_over && over_is(OVER_SCORES)) {
        app_new_game(g_game.rules);         /* "Play Again" */
        return;
    }
    over_pop();
    ui_game_refresh();
}

void app_volume_step(int steps)
{
    sound_set_volume(sound_volume() + steps);
    ui_game_refresh();
}

/* The strength steps apply to the number-finding buzz; a short buzz at the new
 * strength lets the player feel what they chose. */
void app_haptic_step(int steps)
{
    settings_set_haptic(settings_haptic() + steps);
    knob_board_haptic_pulse(haptic_scale(FEEL_FULL, settings_haptic()), 300);
    ui_game_refresh();
}

/* Every buzz that tells the player where the number is goes through here. */
static void buzz(int level, uint32_t hold_ms)
{
    knob_board_haptic_pulse(haptic_scale((uint8_t)level, settings_haptic()), hold_ms);
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
/* A clock that runs out this long after the last touch or turn means the
 * player has walked away. */
#define ABANDON_MS 30000

static void app_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (s_depth) {
        flush_scores();                     /* nothing is animating now */
        ui_game_refresh();                  /* paused: only the menus move */
        return;
    }
    unsigned ev = game_tick(&g_game, app_now_ms());
    if ((ev & GAME_EV_TIMEOUT) && knob_board_idle_ms() >= ABANDON_MS) {
        /* Nobody is there: no siren, and no new game starting itself over and
         * over on a dark screen. Back to the title to wait. */
        ESP_LOGI(TAG, "clock ran out with nobody playing: back to the title");
        knob_board_haptic_pulse(0, 0);
        record_run();
        ev = game_reset(&g_game, app_now_ms());
    }
    handle(ev);
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
    /* The door has finished opening and the prize sound is under way. */
    if (g_game.mode != GAME_UNLOCKED || app_unlock_ms() >= UNLOCK_REWARD_MS + 500) {
        flush_scores();
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
    settings_init();
    game_init(&g_game, seed);
    game_set_rules(&g_game, (game_rules_t)settings_rules());
    ui_game_refresh();
    lv_timer_create(app_timer_cb, 20, NULL);
}
