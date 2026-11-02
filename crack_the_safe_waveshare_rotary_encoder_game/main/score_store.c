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
    snprintf(key, n, "t%d", mode);
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
    score_table_t got;
    size_t len = sizeof(got);
    if (nvs_get_blob(h, key, &got, &len) == ESP_OK && len == sizeof(got) &&
        scores_valid(&got)) {
        *t = got;
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
