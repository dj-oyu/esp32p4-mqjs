/*
 * Persist the most recently verified JS task to LittleFS so it survives
 * a power cycle. Only signature-verified scripts ever reach here (see
 * task_source.c), so the stored file is trusted on the next boot without
 * re-verification.
 */
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include "esp_littlefs.h"
#include "esp_log.h"
#include "storage.h"

#define MOUNT     "/littlefs"
#define TASK_PATH MOUNT "/task.js"
#define MAX_TASK  (64 * 1024)

static const char *TAG = "storage";
static bool s_mounted;

bool storage_init(void)
{
    esp_vfs_littlefs_conf_t conf = {
        .base_path = MOUNT,
        .partition_label = "storage",
        .format_if_mount_failed = true,
        .dont_mount = false,
    };
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "littlefs mount failed: %s", esp_err_to_name(err));
        return false;
    }
    s_mounted = true;
    ESP_LOGI(TAG, "littlefs mounted at %s", MOUNT);
    return true;
}

char *storage_load_task(size_t *len)
{
    *len = 0;
    if (!s_mounted)
        return NULL;
    FILE *f = fopen(TASK_PATH, "rb");
    if (!f)
        return NULL;

    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > MAX_TASK) {
        fclose(f);
        return NULL;
    }
    char *buf = malloc((size_t)n + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t rd = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[rd] = '\0';
    *len = rd;
    ESP_LOGI(TAG, "loaded persisted task (%zu bytes)", rd);
    return buf;
}

void storage_save_task(const char *src, size_t len)
{
    if (!s_mounted)
        return;
    FILE *f = fopen(TASK_PATH, "wb");
    if (!f) {
        ESP_LOGE(TAG, "cannot open %s for writing", TASK_PATH);
        return;
    }
    size_t wr = fwrite(src, 1, len, f);
    fclose(f);
    if (wr != len)
        ESP_LOGE(TAG, "short write (%zu/%zu)", wr, len);
    else
        ESP_LOGI(TAG, "persisted task (%zu bytes)", len);
}

bool storage_save_app(const char *name, const char *src, size_t len)
{
    if (!s_mounted)
        return false;
    mkdir(MOUNT "/apps", 0777); /* EEXIST is fine */
    char path[64];
    snprintf(path, sizeof path, MOUNT "/apps/%s.js", name);
    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "cannot open %s for writing", path);
        return false;
    }
    size_t wr = fwrite(src, 1, len, f);
    fclose(f);
    if (wr != len) {
        ESP_LOGE(TAG, "short write (%zu/%zu)", wr, len);
        return false;
    }
    ESP_LOGI(TAG, "installed app '%s' (%zu bytes)", name, len);
    return true;
}
