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
#include "fs_core.h"
#include "mqjs_runtime.h"
#include "storage.h"

#define MOUNT     "/littlefs"
#define TASK_PATH MOUNT "/task.js"
/* same ceiling the MQTT rx path enforces — a file that arrives must be
   loadable again after a reboot (mqjs_runtime.h) */
#define MAX_TASK  MQJS_SCRIPT_MAX

static const char *TAG = "storage";
static bool s_mounted;

/* ---- fs_core への登録 -------------------------------------------- */

static esp_err_t lfs_usage(const fsvol_t *v, uint64_t *total, uint64_t *freeb)
{
    (void)v;
    size_t t = 0, used = 0;
    esp_err_t err = esp_littlefs_info("storage", &t, &used);
    if (err != ESP_OK)
        return err;
    *total = t;
    *freeb = (t > used) ? t - used : 0;
    return ESP_OK;
}

/* mount/unmount/probe を持たない = 常時マウント済みの固定ボリューム。
   littlefs は起動時に一度マウントされたら外れない (パーティションは
   抜けない) ので、抜き挿しの機構は要らない。 */
static const fsvol_ops_t s_internal_ops = {
    .usage = lfs_usage,
};

static const fsvol_t s_internal_vol = {
    .id     = "internal",
    .label  = "内蔵",
    .root   = MOUNT,
    .fstype = "littlefs",
    .flags  = FSVOL_SYSTEM,
    .ops    = &s_internal_ops,
};

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
    /* ファイラから見える "internal" ボリュームとして公開する。ここより
       上の層 (fs.* バインディングも files.js も) は /littlefs という実パスを
       一度も知らない — docs/filer-storage-design.md §4。 */
    fsvol_register(&s_internal_vol);
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

char *storage_load_app(const char *name, size_t *len)
{
    *len = 0;
    if (!s_mounted)
        return NULL;
    char path[64];
    snprintf(path, sizeof path, MOUNT "/apps/%s.js", name);
    FILE *f = fopen(path, "rb");
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
    return buf;
}

bool storage_delete_app(const char *name)
{
    if (!s_mounted)
        return false;
    char path[64];
    snprintf(path, sizeof path, MOUNT "/apps/%s.js", name);
    if (remove(path) != 0)
        return false;
    ESP_LOGI(TAG, "uninstalled app '%s'", name);
    return true;
}

/* apps/ へ書ける唯一の経路。fopen/fwrite を直に使っているのは手抜きでは
   なく、これが**特権書き込み**だからで、fs_core を通してはいけない
   (通すと fs_path_reserved に弾かれる —— 弾くのが正しい)。
 *
 * 逆から言うと: このファイルの冒頭が主張する「apps/ に届く経路は署名
 * 検証済みしか無い」は、fs_core 側が /internal/apps 以下の変更を全部
 * 拒むことと、ここが唯一の抜け道であることの二枚で成り立っている。
 * 「fs_core に揃えよう」と親切心で書き換えると、アプリが 1 本も
 * インストールできなくなる。 */
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
