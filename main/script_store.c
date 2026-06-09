#include <stdlib.h>
#include "nvs.h"
#include "esp_log.h"
#include "script_store.h"
#include "script_runner.h"

static const char *TAG = "store";

#define STORE_NS  "mqjs"
#define STORE_KEY "script"

char *script_store_load(size_t *len)
{
    nvs_handle_t h;
    if (nvs_open(STORE_NS, NVS_READONLY, &h) != ESP_OK)
        return NULL;

    char *buf = NULL;
    size_t sz = 0;
    esp_err_t err = nvs_get_blob(h, STORE_KEY, NULL, &sz);
    if (err != ESP_OK || sz == 0 || sz > MQJS_SCRIPT_MAX)
        goto out;

    buf = script_psram_alloc(sz + 1);
    if (!buf)
        goto out;
    err = nvs_get_blob(h, STORE_KEY, buf, &sz);
    if (err != ESP_OK) {
        free(buf);
        buf = NULL;
        goto out;
    }
    buf[sz] = '\0';
    *len = sz;
out:
    nvs_close(h);
    return buf;
}

esp_err_t script_store_save(const char *src, size_t len)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(STORE_NS, NVS_READWRITE, &h);
    if (err != ESP_OK)
        return err;
    err = nvs_set_blob(h, STORE_KEY, src, len);
    if (err == ESP_OK)
        err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK)
        ESP_LOGE(TAG, "save failed: %s", esp_err_to_name(err));
    else
        ESP_LOGI(TAG, "script persisted (%u bytes)", (unsigned)len);
    return err;
}

void script_store_erase(void)
{
    nvs_handle_t h;
    if (nvs_open(STORE_NS, NVS_READWRITE, &h) != ESP_OK)
        return;
    nvs_erase_key(h, STORE_KEY);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "stored script erased");
}
