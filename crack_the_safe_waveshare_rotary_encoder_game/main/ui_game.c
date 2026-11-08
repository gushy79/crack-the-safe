/*
 * The Play and Unlocked screens, painted from g_game through ui_lv.h's
 * change-only setters -- an unchanged value is a no-op, so a screen redraws
 * only when something on it actually changes.
 *
 * The dial's rotation is set here too, like Tiercel's clock hands: a live
 * value on a widget whose authored rotation (270, the dial at 0) is what the
 * canvas shows. It is not an EEZ expression binding because EEZ 0.29 reads a
 * bound Scale rotation back with lv_scale_get_rotation(), which LVGL 9.2.2
 * does not have.
 */

#include "ui_game.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app.h"
#include "bt_link.h"
#include "knob_board.h"
#include "screens.h"
#include "settings.h"
#include "sound.h"
#include "wifi_link.h"
#include "ui_lv.h"

static const lv_state_t DONE  = LV_STATE_CHECKED;
static const lv_state_t WRONG = LV_STATE_DISABLED;
static const lv_state_t CURRENT = LV_STATE_PRESSED;

/* ---- Play ------------------------------------------------------------------ */

#define POLICE_SWAP_MS 350

/* The battery, above "Level N": how full, as Font Awesome's five battery
 * glyphs, red when low (BatteryIcon DISABLED). On USB power a bolt sits in
 * front, green (CHECKED), and the bars fill up over and over -- the board
 * cannot see the cell while it charges, so there is no level to show. */
#define BATT_LOW_PCT     15
#define BATT_HYST_PCT    4      /* a reading must clear an edge by this to go up */
#define BATT_FILL_STEP_MS 600

/* UTF-8 for U+F244 (empty) .. U+F240 (full), and U+F0E7, the bolt -- all in
 * the fa14 range. */
static const char *const BATT_GLYPH[5] = {
    "\xEF\x89\x84", "\xEF\x89\x83", "\xEF\x89\x82", "\xEF\x89\x81", "\xEF\x89\x80",
};
#define BOLT_GLYPH "\xEF\x83\xA7"

/* Bars 0..4 from the level. The voltage climbs back a little whenever the
 * motor or backlight eases off, so going UP a bar, or out of low, needs a
 * margin -- or the icon would flick between two bars all game. */
static int battery_bars(int pct, bool *low)
{
    static const uint8_t EDGE[4] = { 10, 30, 55, 80 };   /* bars 1..4 from here */
    static int  s_bars = -1;
    static bool s_low;

    int bars = 0;
    while (bars < 4 && pct >= EDGE[bars]) {
        bars++;
    }
    if (s_bars >= 0 && bars == s_bars + 1 && pct < EDGE[s_bars] + BATT_HYST_PCT) {
        bars = s_bars;
    }
    s_bars = bars;

    s_low = s_low ? pct < BATT_LOW_PCT + BATT_HYST_PCT : pct <= BATT_LOW_PCT;
    *low = s_low;
    return bars;
}

static void refresh_battery(void)
{
    knob_board_battery_t b;
    const bool known = knob_board_battery_get(&b);
    ui_lv_set_hidden(objects.battery_icon, !known);
    if (!known) {
        return;
    }

    char buf[8];
    if (b.charging) {
        const int bars = (int)((app_now_ms() / BATT_FILL_STEP_MS) % 5);
        snprintf(buf, sizeof(buf), BOLT_GLYPH "%s", BATT_GLYPH[bars]);
        ui_lv_set_text(objects.battery_icon, buf);
        ui_lv_set_state_in(objects.battery_icon, DONE | WRONG, DONE);
        return;
    }
    bool low;
    const int bars = battery_bars(b.percent, &low);
    ui_lv_set_text(objects.battery_icon, BATT_GLYPH[bars]);
    ui_lv_set_state_in(objects.battery_icon, DONE | WRONG, low ? WRONG : 0);
}

/* Turn the numbered ring and the knob's grip ring with the knob. A Scale's
 * rotation is where its first tick (0) sits; 270 = 12 o'clock, and each
 * detent is one position, 9 degrees. */
static void set_dial_rotation(void)
{
    static int32_t s_shown = -1;
    int32_t r = (270 + g_game.ticks * GAME_DEG_PER_POS) % 360;
    if (r < 0) {
        r += 360;
    }
    if (r != s_shown) {
        lv_scale_set_rotation(objects.dial_ticks, r);
        lv_scale_set_rotation(objects.dial_grip, r);
        s_shown = r;
    }
}

static void refresh_play(void)
{
    const bool wrong = g_game.mode == GAME_WRONG;
    /* Zen's miss is calm: no red, no police lights; "wrong" is still true for
     * the text and arrows, "harsh" is for the alarming colours. */
    const bool harsh = wrong && g_game.rules != GAME_RULES_ZEN;
    const int  dir = game_stage_dir(&g_game);
    char buf[40];

    set_dial_rotation();

    /* The countdown, rounded UP so "0:00" only shows once time is out, and
     * red for the last ten seconds as well as in the red hold. */
    const uint32_t left_ms = game_time_left_ms(&g_game, app_now_ms());
    const uint32_t left_s = (left_ms + 999) / 1000;
    snprintf(buf, sizeof(buf), "%lu:%02lu", (unsigned long)(left_s / 60),
             (unsigned long)(left_s % 60));
    ui_lv_set_text(objects.timer_text, buf);
    ui_lv_set_state_in(objects.timer_text, WRONG, (harsh || left_s <= 10) ? WRONG : 0);
    /* Zen has no clock to show. */
    const bool timed = game_timed(&g_game);
    ui_lv_set_hidden(objects.timer_text, !timed);

    char loop[16] = "";
    if (g_game.loops > 0) {
        snprintf(loop, sizeof(loop), " \xC2\xB7 loop %u", g_game.loops + 1u);
    }
    snprintf(buf, sizeof(buf), "%sLevel %u%s", timed ? "" : "Zen \xC2\xB7 ",
             g_game.level ? g_game.level : 1, loop);
    ui_lv_set_text(objects.level_text, buf);

    /* Lives: one heart (U+F004, icon font) for each chance left. */
    char hearts[16] = "";
    if (g_game.rules == GAME_RULES_LIVES) {
        for (unsigned n = 0; n < g_game.lives && n < GAME_LIVES; n++) {
            strcat(hearts, "\xEF\x80\x84");
        }
    }
    ui_lv_set_text(objects.lives_text, hearts);
    refresh_battery();
    ui_lv_set_text(objects.hint_text,
                   wrong ? (g_game.rules == GAME_RULES_ZEN ? "Not quite"
                                : g_game.timed_out ? "Time's Up" : "Wrong")
                         : dir > 0 ? "Turn Right" : "Turn Left");
    ui_lv_set_hidden(objects.hint_arrow_cw, wrong || dir < 0);
    ui_lv_set_hidden(objects.hint_arrow_ccw, wrong || dir > 0);

    ui_lv_set_state_in(objects.hint_text, WRONG, harsh ? WRONG : 0);
    ui_lv_set_state_in(objects.dial_index, WRONG, harsh ? WRONG : 0);

    lv_obj_t *const seg[3]     = { objects.seg1, objects.seg2, objects.seg3 };
    lv_obj_t *const seg_cur[3] = { objects.seg1_cur, objects.seg2_cur, objects.seg3_cur };
    lv_obj_t *const slot[3]    = { objects.slot1, objects.slot2, objects.slot3 };
    lv_obj_t *const line[3]    = { objects.slot1_line, objects.slot2_line, objects.slot3_line };
    lv_obj_t *const cur[3]     = { objects.slot1_cur, objects.slot2_cur, objects.slot3_cur };
    for (int k = 0; k < 3; k++) {
        const bool done = k < g_game.locked;
        const bool current = g_game.mode == GAME_PLAY && k == g_game.stage;
        const lv_state_t st = harsh ? WRONG : done ? DONE : 0;
        ui_lv_set_state_in(seg[k], DONE | WRONG, st);
        /* The slot being searched is white in the design: PRESSED, borrowed
         * as a selector on an inert label (SlotText). */
        ui_lv_set_state_in(slot[k], DONE | WRONG | CURRENT,
                           st ? st : current ? CURRENT : 0);
        ui_lv_set_state_in(line[k], DONE | WRONG, st);
        ui_lv_set_hidden(seg_cur[k], !current);
        ui_lv_set_hidden(cur[k], !current);
        if (done) {
            snprintf(buf, sizeof(buf), "%02u", g_game.code[k]);
            ui_lv_set_text(slot[k], buf);
        } else {
            ui_lv_set_text(slot[k], "--");
        }
    }

    /* The police are coming: red and blue light swapping over the dial for
     * the whole "wrong" hold. It swaps, never blinks to dark, and stays under
     * three changes a second -- big saturated flashes faster than that are a
     * photosensitivity risk. */
    const uint32_t t = app_now_ms() - g_game.mode_since_ms;
    const bool red = (t / POLICE_SWAP_MS) % 2 == 0;
    ui_lv_set_hidden(objects.police_red, !(harsh && red));
    /* Zen's miss is only a soft blue wash over the dial that swells and fades
     * again over the calm hold: a cue for when the sound is off. */
    const bool calm = wrong && !harsh;
    ui_lv_set_hidden(objects.police_blue, !((harsh && !red) || calm));
    if (calm) {
        const uint32_t hold = GAME_ZEN_HOLD_MS;
        const uint32_t peak = hold / 4;
        const uint32_t k = t < peak ? t * 100 / peak
                                    : t < hold ? (hold - t) * 100 / (hold - peak) : 0;
        lv_obj_set_style_opa(objects.police_blue, (lv_opa_t)(k * 90 / 100), 0);
    } else {
        lv_obj_remove_local_style_prop(objects.police_blue, LV_STYLE_OPA, 0);
    }
}

/* ---- Unlocked ---------------------------------------------------------------- */

static lv_obj_t *door_frame(int i)
{
    lv_obj_t *const frames[UNLOCK_DOOR_FRAMES] = {
        objects.door_00, objects.door_01, objects.door_02, objects.door_03,
        objects.door_04, objects.door_05, objects.door_06, objects.door_07,
        objects.door_08, objects.door_09, objects.door_10, objects.door_11,
        objects.door_12, objects.door_13, objects.door_14, objects.door_15,
    };
    return frames[i];
}

/* The glow's brightness steps, as GoldGlow style states: DISABLED (dim),
 * CHECKED (mid), DEFAULT (full). It fades in dim-mid-full, then breathes
 * full-mid-dim-mid every GLOW_STEP_MS. */
#define GLOW_STEP_MS 260

static lv_state_t glow_state(uint32_t since_glow)
{
    const uint32_t step = since_glow / GLOW_STEP_MS;
    if (step < 2) {
        return step == 0 ? LV_STATE_DISABLED : LV_STATE_CHECKED;
    }
    static const lv_state_t breathe[4] = { 0, LV_STATE_CHECKED, LV_STATE_DISABLED,
                                           LV_STATE_CHECKED };
    return breathe[(step - 2) % 4];
}

static lv_obj_t *prize(int level)
{
    lv_obj_t *const prizes[GAME_LEVELS] = {
        objects.prize_1, objects.prize_2, objects.prize_3, objects.prize_4,
        objects.prize_5, objects.prize_6, objects.prize_7, objects.prize_8,
    };
    return prizes[level - 1];
}

static void refresh_unlocked(void)
{
    const bool open = g_game.mode == GAME_UNLOCKED;
    const uint32_t t = open ? app_unlock_ms() : 0;

    /* The prize behind the door is the level's own. */
    const int lv = g_game.level ? g_game.level : 1;
    for (int n = 1; n <= GAME_LEVELS; n++) {
        ui_lv_set_hidden(prize(n), n != lv);
    }
    const int frame = app_door_frame_at(t);
    for (int i = 0; i < UNLOCK_DOOR_FRAMES; i++) {
        ui_lv_set_hidden(door_frame(i), i != frame);
    }

    const uint32_t glow_at = UNLOCK_DOOR_START_MS + (UNLOCK_GLOW_FRAME - 1) * UNLOCK_FRAME_MS;
    const bool glow = open && t >= glow_at;
    ui_lv_set_hidden(objects.gold_glow, !glow);
    if (glow) {
        ui_lv_set_state_in(objects.gold_glow, LV_STATE_CHECKED | LV_STATE_DISABLED,
                           glow_state(t - glow_at));
    }

    ui_lv_set_hidden(objects.reward_pill, !(open && t >= UNLOCK_REWARD_MS));
    ui_lv_set_hidden(objects.reward_again, !(open && t >= GAME_REPLAY_LOCKOUT_MS));

    char buf[40];
    /* U+00B7 middle dot is in the house Roboto range. */
    snprintf(buf, sizeof(buf), "%02u \xC2\xB7 %02u \xC2\xB7 %02u",
             g_game.code[0], g_game.code[1], g_game.code[2]);
    ui_lv_set_text(objects.reward_code, buf);
    const uint32_t s = (game_time_left_ms(&g_game, app_now_ms()) + 999) / 1000;
    if (game_timed(&g_game)) {
        snprintf(buf, sizeof(buf), "Level %u \xC2\xB7 %lu:%02lu left", g_game.level,
                 (unsigned long)(s / 60), (unsigned long)(s % 60));
    } else {
        snprintf(buf, sizeof(buf), "Level %u", g_game.level);
    }
    ui_lv_set_text(objects.reward_meta, buf);
    /* The run's score so far; it goes in the table when the run ends. */
    char num[16];
    scores_fmt_thousands(game_run_score(&g_game), num, sizeof(num));
    snprintf(buf, sizeof(buf), "Run %s", num);
    if (g_game.rules == GAME_RULES_ZEN) {
        snprintf(buf, sizeof(buf), "Zen \xC2\xB7 no score");
    }
    ui_lv_set_text(objects.reward_score, buf);
    const int next = game_next_level(&g_game);
    if (next == 1) {
        ui_lv_set_text(objects.reward_again, "Turn to Play Again");
    } else {
        snprintf(buf, sizeof(buf), "Turn for Level %d", next);
        ui_lv_set_text(objects.reward_again, buf);
    }
}

/* ---- Instructions and Headphones ------------------------------------------- */

/* One list row's pitch: 34 px row + 4 px gap (screens_layout.py). */
#define HP_ROW_PITCH 38

static bool s_info_open;     /* the (i) panel: Forget Device / Cancel */
static bool s_info_dirty;

void ui_game_headphones_info(bool open)
{
    s_info_open = open;
    s_info_dirty = true;
}

static void refresh_headphones(void)
{
    static uint32_t s_gen = UINT32_MAX;
    bt_link_state_t s;
    bt_link_get(&s);
    if (s.gen == s_gen && !s_info_dirty) {
        return;
    }
    s_gen = s.gen;
    s_info_dirty = false;

    const bool connected = s.present && s.conn == BT_CONN_CONNECTED;
    bt_dev_t home;
    const bool has_home = s.present && bt_link_home(&s, &home);

    /* The Instructions page's button goes green while connected. */
    const lv_state_t on = connected ? DONE : 0;
    ui_lv_set_state_in(objects.hp_button, DONE, on);
    ui_lv_set_state_in(objects.hp_button_icon, DONE, on);
    ui_lv_set_state_in(objects.hp_button_text, DONE, on);

    /* Settings: the Headphones row's value. */
    ui_lv_set_text(objects.settings_hp_value,
                   !s.present ? "Unavailable" : connected ? "Connected" : "Not connected");
    ui_lv_set_state_in(objects.settings_hp_value, DONE | WRONG,
                       !s.present ? WRONG : connected ? DONE : 0);

    const char *status;
    bool bad = false;
    if (!s.present) {
        status = "Bluetooth is not available";
        bad = true;
    } else if (s.conn == BT_CONN_CONNECTING) {
        status = "Connecting...";
    } else if (connected) {
        status = "Connected";
    } else if (s.failed) {
        status = "Couldn't connect. Try again.";
        bad = true;
    } else if (has_home || s.ndev > 0) {
        status = "Tap your headphones";
    } else if (s.searching) {
        status = "Searching...";
    } else {
        status = "Put headphones in pairing mode";
    }
    ui_lv_set_text(objects.hp_status, status);
    ui_lv_set_state_in(objects.hp_status, WRONG, bad ? WRONG : 0);

    lv_obj_t *const rows[BT_LINK_ROWS] = {
        objects.hp_row0, objects.hp_row1, objects.hp_row2, objects.hp_row3,
        objects.hp_row4, objects.hp_row5, objects.hp_row6, objects.hp_row7,
    };
    lv_obj_t *const texts[BT_LINK_ROWS] = {
        objects.hp_row0_text, objects.hp_row1_text, objects.hp_row2_text, objects.hp_row3_text,
        objects.hp_row4_text, objects.hp_row5_text, objects.hp_row6_text, objects.hp_row7_text,
    };
    for (int i = 0; i < BT_LINK_ROWS; i++) {
        bt_dev_t d;
        const bool shown = s.present && bt_link_row(&s, i, &d);
        ui_lv_set_hidden(rows[i], !shown);
        if (shown) {
            ui_lv_set_text(texts[i], d.name);
            ui_lv_set_state_in(texts[i], DONE, i == 0 && connected ? DONE : 0);
        }
    }
    /* (i) only on the remembered headphones, which are always row 0. */
    ui_lv_set_hidden(objects.hp_row0_info, !has_home);

    const bool info = s_info_open && has_home;
    ui_lv_set_hidden(objects.hp_info, !info);
    if (info) {
        ui_lv_set_text(objects.hp_info_name, home.name);
    }
}

void ui_game_scroll_headphones(int detents)
{
    lv_obj_scroll_by_bounded(objects.hp_list, 0, -detents * HP_ROW_PITCH, LV_ANIM_ON);
}

/* ---- Settings -------------------------------------------------------------- */

/* The Volume row: the level, and - / + greyed out at the ends of the range
 * (DISABLED also stops them taking taps). */
static void refresh_settings(void)
{
    const int v = sound_volume();
    char buf[8];
    if (v == 0) {
        snprintf(buf, sizeof(buf), "Off");
    } else {
        snprintf(buf, sizeof(buf), "%d", v);
    }
    ui_lv_set_text(objects.settings_vol_value, buf);
    const lv_state_t at_min = v == 0 ? WRONG : 0;
    const lv_state_t at_max = v == SOUND_VOLUME_MAX ? WRONG : 0;
    ui_lv_set_state_in(objects.settings_vol_down, WRONG, at_min);
    ui_lv_set_state_in(objects.settings_vol_down_icon, WRONG, at_min);
    ui_lv_set_state_in(objects.settings_vol_up, WRONG, at_max);
    ui_lv_set_state_in(objects.settings_vol_up_icon, WRONG, at_max);

    const int hs = settings_haptic();
    snprintf(buf, sizeof(buf), "%d", hs);
    ui_lv_set_text(objects.settings_hap_value, buf);
    const lv_state_t h_min = hs == HAPTIC_MIN ? WRONG : 0;
    const lv_state_t h_max = hs == HAPTIC_MAX ? WRONG : 0;
    ui_lv_set_state_in(objects.settings_hap_down, WRONG, h_min);
    ui_lv_set_state_in(objects.settings_hap_down_icon, WRONG, h_min);
    ui_lv_set_state_in(objects.settings_hap_up, WRONG, h_max);
    ui_lv_set_state_in(objects.settings_hap_up_icon, WRONG, h_max);
}

/* ---- Wi-Fi ---------------------------------------------------------------------- */

/* The Settings row's value, and the setup page: a QR code that joins the board's
 * hotspot (the phone then opens its page), then what is happening. The QR is
 * made on first use and updated only when its text changes. */
static lv_obj_t *s_qr;

static void refresh_wifi(void)
{
    const wifi_link_state_t st = wifi_link_state();
    const bool time_ok = wifi_link_time_valid();
    ui_lv_set_text(objects.settings_wifi_value,
                   st == WIFI_LINK_PROVISIONING || st == WIFI_LINK_STARTING ? "Setting up" :
                   st == WIFI_LINK_CONNECTING   ? "Connecting" :
                   st == WIFI_LINK_FAILED       ? "Can't connect" :
                   time_ok                      ? wifi_link_saved_ssid() :   /* joined at boot */
                   wifi_link_has_network()      ? "Not connected" : "Not set up");
    /* The play page's symbol beside the battery: only once a network is saved, and
     * lit while the clock has been set from it this boot. */
    ui_lv_set_hidden(objects.play_wifi_icon, !wifi_link_has_network());
    /* With the symbol showing, the pair is centred: the battery steps left to make room. */
    lv_obj_set_x(objects.battery_icon, wifi_link_has_network() ? 146 : 160);
    ui_lv_set_state_in(objects.play_wifi_icon, LV_STATE_CHECKED, time_ok ? LV_STATE_CHECKED : 0);

    const bool setup = st == WIFI_LINK_PROVISIONING && !wifi_link_provision_done();
    if (setup && !s_qr) {
        s_qr = lv_qrcode_create(objects.page_wifi);
        lv_qrcode_set_size(s_qr, 116);
        lv_qrcode_set_dark_color(s_qr, lv_color_black());
        lv_qrcode_set_light_color(s_qr, lv_color_white());
        lv_obj_set_style_border_color(s_qr, lv_color_white(), 0);   /* the quiet zone */
        lv_obj_set_style_border_width(s_qr, 4, 0);
        lv_obj_set_pos(s_qr, 118, 84);
    }
    if (s_qr) {
        ui_lv_set_hidden(s_qr, !setup);
    }
    if (setup) {
        char ssid[33], pass[12], text[80];
        static char s_last[80];
        wifi_link_provision_info(ssid, sizeof(ssid), pass, sizeof(pass));
        if (wifi_link_provision_clients() > 0) {      /* a phone is on: now the page */
            snprintf(text, sizeof(text), "http://192.168.4.1/");
        } else {
            snprintf(text, sizeof(text), "WIFI:T:WPA;S:%s;P:%s;;", ssid, pass);
        }
        if (strcmp(text, s_last)) {
            lv_qrcode_update(s_qr, text, (uint32_t)strlen(text));
            snprintf(s_last, sizeof(s_last), "%s", text);
        }
    }

    char buf[80];
    const char *msg;
    if (st == WIFI_LINK_STARTING) {
        msg = "Starting Wi-Fi...";
    } else if (setup && wifi_link_provision_clients() > 0) {
        msg = "Connected. Scan again to\nopen the setup page";
    } else if (setup) {
        msg = "Scan with your phone\nto join the safe's Wi-Fi";
    } else if (st == WIFI_LINK_PROVISIONING || st == WIFI_LINK_CONNECTING) {
        snprintf(buf, sizeof(buf), "Joining %s...", wifi_link_saved_ssid());
        msg = buf;
    } else if (st == WIFI_LINK_FAILED) {
        msg = "Couldn't join.\nCheck the password.";
    } else if (time_ok) {
        msg = "Connected.\nThe clock is set.";
    } else {
        msg = "Not connected.";
    }
    ui_lv_set_text(objects.wifi_status, msg);
    ui_lv_set_text(objects.wifi_back_text,
                   setup || st == WIFI_LINK_STARTING ? "Cancel" : "Done");
}

/* ---- Top Scores ------------------------------------------------------------ */

/* Five rows, and a sixth when the latest entry is further down than that
 * (with its true place). A row is a whole run: "1.  192,345 \xC2\xB7 19 lvls \xC2\xB7
 * 2 loops \xC2\xB7 3 fails" (no fails in Lives). "--" for an empty place. The latest entry is gold; after a game over the page says so, and
 * its button plays again. */
#define NEW_ENTRY_COLOR 0xF2C14E

/* Fails show in Classic only: in Lives a lost life is already in the score. */
static void fmt_entry(char *out, unsigned n, const char *place, score_entry_t e, bool lives)
{
    char body[56];
    scores_fmt_run(e, !lives, body, sizeof(body));
    snprintf(out, n, "%s  %s", place, body);
}

static void set_gold(lv_obj_t *row, bool gold)
{
    if (gold) {
        lv_obj_set_style_text_color(row, lv_color_hex(NEW_ENTRY_COLOR), 0);
    } else {
        lv_obj_remove_local_style_prop(row, LV_STYLE_TEXT_COLOR, 0);
    }
}

static int s_detail = -1;       /* the Scores row whose detail is open, or -1 */

void ui_game_score_detail(int row)
{
    s_detail = row;
}

static void refresh_scores(void)
{
    lv_obj_t *const rows[SCORES_SHOWN] = {
        objects.scores_row_1, objects.scores_row_2, objects.scores_row_3,
        objects.scores_row_4, objects.scores_row_5,
    };
    const score_table_t *t = app_scores();
    const bool lives = app_scores_view() == 1;
    const bool over = app_scores_game_over();
    ui_lv_set_text(objects.scores_title,
                   over ? "Game Over" : lives ? "Lives Scores" : "Classic Scores");
    ui_lv_set_hidden(objects.scores_arrow_l, over);       /* nothing to flip */
    ui_lv_set_hidden(objects.scores_arrow_r, over);
    ui_lv_set_text(objects.scores_back_text, over ? "Play Again" : "Back");

    score_entry_t latest;
    int rank = -1;
    const bool have = app_scores_latest(&latest, &rank);
    for (int i = 0; i < SCORES_SHOWN; i++) {
        char buf[80], place[8];
        snprintf(place, sizeof(place), "%d.", i + 1);
        if (i < t->count) {
            fmt_entry(buf, sizeof(buf), place, t->e[i], lives);
        } else {
            snprintf(buf, sizeof(buf), "%s  --", place);
        }
        ui_lv_set_text(rows[i], buf);
        set_gold(rows[i], have && rank == i);
    }

    /* Below the top five: the latest entry on its own line, with its place. */
    const bool extra = have && (rank < 0 || rank >= SCORES_SHOWN);
    ui_lv_set_hidden(objects.scores_row_6, !extra);
    if (extra) {
        char buf[80], place[12];
        if (rank < 0) {
            snprintf(place, sizeof(place), ">%d.", SCORES_MAX);
        } else {
            snprintf(place, sizeof(place), "%d.", rank + 1);
        }
        fmt_entry(buf, sizeof(buf), place, latest, lives);
        ui_lv_set_text(objects.scores_row_6, buf);
        set_gold(objects.scores_row_6, true);
    }

    /* A row's detail panel. */
    score_entry_t pick = { 0 };
    char place[12] = "";
    if (s_detail >= 0 && s_detail < SCORES_SHOWN && s_detail < t->count) {
        pick = t->e[s_detail];
        snprintf(place, sizeof(place), "%d", s_detail + 1);
    } else if (s_detail == SCORES_SHOWN && extra) {
        pick = latest;
        if (rank < 0) {
            snprintf(place, sizeof(place), ">%d", SCORES_MAX);
        } else {
            snprintf(place, sizeof(place), "%d", rank + 1);
        }
    }
    const bool detail = pick.score != 0;
    if (!detail) {
        s_detail = -1;
    }
    ui_lv_set_hidden(objects.sd_detail, !detail);
    if (detail) {
        char num[16], when[40], buf2[64];
        scores_fmt_thousands(pick.score, num, sizeof(num));
        snprintf(buf2, sizeof(buf2), "Place %s \xC2\xB7 %s", place, num);
        ui_lv_set_text(objects.sd_title, buf2);
        scores_fmt_when(pick.when, when, sizeof(when));
        ui_lv_set_text(objects.sd_when, when);
        const unsigned loops = pick.levels / GAME_LEVELS;
        int n = snprintf(buf2, sizeof(buf2), "%u %s", (unsigned)pick.levels,
                         pick.levels == 1 ? "safe" : "safes");
        if (loops > 0) {
            n += snprintf(buf2 + n, sizeof(buf2) - (unsigned)n, " \xC2\xB7 %u %s", loops,
                          loops == 1 ? "loop" : "loops");
        }
        if (!lives) {
            snprintf(buf2 + n, sizeof(buf2) - (unsigned)n, " \xC2\xB7 %u %s",
                     (unsigned)pick.fails, pick.fails == 1 ? "fail" : "fails");
        }
        ui_lv_set_text(objects.sd_stats, buf2);
    }
}

void ui_game_refresh(void)
{
    refresh_play();
    refresh_unlocked();
    refresh_headphones();
    refresh_wifi();
    refresh_settings();
    refresh_scores();
}
