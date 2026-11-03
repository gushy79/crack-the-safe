/*
 * Player settings in NVS. See settings.h.
 */

#include "settings.h"

#include "esp_log.h"
#include "game.h"
#include "nvs.h"

static const char *TAG = "settings";

#define NVS_NS "settings"

static int s_haptic = HAPTIC_DEFAULT;
static int s_rules = GAME_RULES_CLASSIC;

static void save_u8(const char *key, uint8_t v)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_u8(h, key, v);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s not saved: %s", key, esp_err_to_name(err));
    }
}

void settings_init(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;                         /* first boot: the defaults stand */
    }
    uint8_t v;
    if (nvs_get_u8(h, "haptic", &v) == ESP_OK && v >= HAPTIC_MIN && v <= HAPTIC_MAX) {
        s_haptic = v;
    }
    if (nvs_get_u8(h, "rules", &v) == ESP_OK && v < GAME_RULES_COUNT) {
        s_rules = v;
    }
    nvs_close(h);
}

int settings_haptic(void)
{
    return s_haptic;
}

void settings_set_haptic(int strength)
{
    s_haptic = strength < HAPTIC_MIN ? HAPTIC_MIN : strength > HAPTIC_MAX ? HAPTIC_MAX : strength;
    save_u8("haptic", (uint8_t)s_haptic);
}

int settings_rules(void)
{
    return s_rules;
}

void settings_set_rules(int rules)
{
    if (rules < 0 || rules >= GAME_RULES_COUNT) {
        rules = GAME_RULES_CLASSIC;
    }
    if (rules != s_rules) {
        s_rules = rules;
        save_u8("rules", (uint8_t)s_rules);
    }
}
