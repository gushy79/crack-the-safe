/*
 * The high-score tables in NVS. See score_store.h.
 */

#include "score_store.h"

#include <stdio.h>

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "scores";

#define NVS_NS "scores"

static void key_for(int mode, char *key, size_t n)
{
    snprintf(key, n, mode == 1 ? "l%d" : "c%d", mode);
}

/* The first versions kept only the top five (a 44-byte blob), under "t0" for
 * Classic and, briefly, "r1" for Lives. Read those once so no score is lost. */
typedef struct {
    uint32_t score;
    uint8_t  level;
    uint16_t left_s;
} legacy_entry_t;

typedef struct {
    uint8_t        count;
    legacy_entry_t e[5];
} legacy_table_t;

static void load_legacy(nvs_handle_t h, int mode, score_table_t *t)
{
    legacy_table_t old;
    size_t len = sizeof(old);
    if (nvs_get_blob(h, mode == 1 ? "r1" : "t0", &old, &len) != ESP_OK ||
        len != sizeof(old) || old.count > 5) {
        return;
    }
    for (int i = 0; i < old.count; i++) {
        score_entry_t e = { .score = old.e[i].score, .levels = old.e[i].level,
                            .left_s = old.e[i].left_s };
        scores_insert(t, e);
    }
    if (!scores_valid(t)) {
        scores_clear(t);
    }
}

void score_store_load(int mode, score_table_t *t)
{
    scores_clear(t);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;                        /* first boot: nothing saved yet */
    }
    char key[8];
    key_for(mode, key, sizeof(key));
    static score_table_t got;          /* 2 KB: not on the stack */
    size_t len = sizeof(got);
    const esp_err_t err = nvs_get_blob(h, key, &got, &len);
    if (err == ESP_OK && len == sizeof(got) && scores_valid(&got)) {
        *t = got;
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        load_legacy(h, mode, t);       /* the next save moves it to the new key */
    } else {
        ESP_LOGW(TAG, "mode %d: no usable saved table, starting empty", mode);
    }
    nvs_close(h);
}

bool score_store_save(int mode, const score_table_t *t)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        char key[8];
        key_for(mode, key, sizeof(key));
        err = nvs_set_blob(h, key, t, sizeof(*t));
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mode %d table not saved: %s", mode, esp_err_to_name(err));
        return false;
    }
    return true;
}
