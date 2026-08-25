/*
 * ボリューム登録簿と仮想パスの解決。docs/filer-storage-design.md §4-§5。
 */
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "fs_core.h"

/* 登録されうるボリューム数。今は internal + sd の 2 つで、将来 USB や
   リモート FS が来ても数個。固定配列で足りる (登録簿のために malloc
   したくない: 起動の一番早い時期に呼ばれる)。 */
#define FSVOL_MAX 4

static const char *TAG = "fsvol";

static const fsvol_t *s_vol[FSVOL_MAX];
static bool           s_mounted[FSVOL_MAX];
static int            s_n;
static uint32_t       s_epoch[FSVOL_MAX];

/* 登録簿とマウント状態を守る。JS はひとつのタスクからしか触らないが、
   マウント/アンマウントは UI タスクや起動シーケンスからも来る。 */
static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf;

static void lock_init(void)
{
    if (!s_lock)
        s_lock = xSemaphoreCreateRecursiveMutexStatic(&s_lock_buf);
}

static void lock(void)   { lock_init(); xSemaphoreTakeRecursive(s_lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGiveRecursive(s_lock); }

static int index_of(const fsvol_t *v)
{
    for (int i = 0; i < s_n; i++)
        if (s_vol[i] == v)
            return i;
    return -1;
}

esp_err_t fsvol_register(const fsvol_t *v)
{
    if (!v || !v->id || !v->root || !v->id[0])
        return ESP_ERR_INVALID_ARG;

    lock();
    esp_err_t err = ESP_OK;
    if (s_n >= FSVOL_MAX) {
        err = ESP_ERR_NO_MEM;
    } else if (fsvol_find(v->id)) {
        err = ESP_ERR_INVALID_STATE;
    } else {
        s_vol[s_n] = v;
        /* ops が無いボリュームは「常時マウント済み」。呼び出し側は
           マウントに成功してから登録する契約 (main/storage.c)。 */
        s_mounted[s_n] = (v->ops == NULL || v->ops->mount == NULL);
        s_epoch[s_n]   = 1;
        s_n++;
        ESP_LOGI(TAG, "volume '%s' (%s) at %s", v->id, v->fstype ? v->fstype : "?",
                 v->root);
    }
    unlock();
    return err;
}

int fsvol_count(void)
{
    lock();
    int n = s_n;
    unlock();
    return n;
}

const fsvol_t *fsvol_at(int i)
{
    lock();
    const fsvol_t *v = (i >= 0 && i < s_n) ? s_vol[i] : NULL;
    unlock();
    return v;
}

const fsvol_t *fsvol_find(const char *id)
{
    if (!id)
        return NULL;
    lock();
    const fsvol_t *found = NULL;
    for (int i = 0; i < s_n; i++)
        if (strcmp(s_vol[i]->id, id) == 0) {
            found = s_vol[i];
            break;
        }
    unlock();
    return found;
}

uint32_t fsvol_epoch(const fsvol_t *v)
{
    lock();
    int idx = index_of(v);
    uint32_t e = (idx >= 0) ? s_epoch[idx] : 0;
    unlock();
    return e;
}

/* ロックを持った状態で呼ぶ。probe が「もう居ない」と言ったら、
   ここでアンマウントまで済ませる — 抜かれたカードのマウント状態を
   引きずったまま open すると VFS 層で長いタイムアウトを食う。 */
static bool mounted_locked(int idx)
{
    if (!s_mounted[idx])
        return false;
    const fsvol_t *v = s_vol[idx];
    if (v->ops && v->ops->probe && !v->ops->probe(v)) {
        ESP_LOGW(TAG, "volume '%s' vanished — unmounting", v->id);
        if (v->ops->unmount)
            v->ops->unmount(v);
        s_mounted[idx] = false;
        return false;
    }
    return true;
}

bool fsvol_mounted(const fsvol_t *v)
{
    lock();
    int idx = index_of(v);
    bool m = (idx >= 0) && mounted_locked(idx);
    unlock();
    return m;
}

esp_err_t fsvol_mount(const fsvol_t *v)
{
    lock();
    esp_err_t err;
    int idx = index_of(v);
    if (idx < 0) {
        err = ESP_ERR_NOT_FOUND;
    } else if (s_mounted[idx]) {
        err = ESP_OK;                       /* 冪等 */
    } else if (!v->ops || !v->ops->mount) {
        err = ESP_ERR_NOT_SUPPORTED;
    } else {
        err = v->ops->mount(v);
        if (err == ESP_OK) {
            s_mounted[idx] = true;
            s_epoch[idx]++;   /* 抜き差しをまたいだ権限を無効にする番号 */
            ESP_LOGI(TAG, "volume '%s' mounted", v->id);
        }
    }
    unlock();
    return err;
}

esp_err_t fsvol_unmount(const fsvol_t *v)
{
    lock();
    esp_err_t err = ESP_OK;
    int idx = index_of(v);
    if (idx < 0) {
        err = ESP_ERR_NOT_FOUND;
    } else if (!s_mounted[idx]) {
        err = ESP_OK;                       /* 冪等 */
    } else if (!v->ops || !v->ops->unmount) {
        err = ESP_ERR_NOT_SUPPORTED;
    } else {
        err = v->ops->unmount(v);
        s_mounted[idx] = false;             /* 失敗しても状態は落とす:
                                               握ったままにする方が危険 */
        ESP_LOGI(TAG, "volume '%s' unmounted", v->id);
    }
    unlock();
    return err;
}

/* ---- 仮想パス --------------------------------------------------- */

/* 受け付ける形: "/id" または "/id/以下"。
 *
 * ここで弾いておけば、この先どのコードも ".." を気にしなくてよい。
 * 正規化 (".." を解決する) ではなく拒否にしているのは、正規化は
 * 「うっかり通してしまう」書き方が多く、拒否は間違えようがないから。
 */
static bool path_shape_ok(const char *p, size_t *id_len_out)
{
    if (!p || p[0] != '/')
        return false;
    size_t n = strnlen(p, FS_PATH_MAX + 1);
    if (n > FS_PATH_MAX)
        return false;

    size_t id_len = 0;
    while (p[1 + id_len] && p[1 + id_len] != '/')
        id_len++;
    if (id_len == 0)
        return false;
    *id_len_out = id_len;

    /* 全体を一度なめて、制御文字・"//"・"." / ".." のセグメントを拒否。 */
    size_t seg = 0;
    bool   all_dots = true;
    for (size_t i = 1; i <= n; i++) {
        char c = p[i];
        if (c == '/' || c == '\0') {
            if (seg == 0 && i != n)     /* "//" — 末尾の '/' だけは許す */
                return false;
            if (seg > 0 && all_dots)    /* "." ".." "..." */
                return false;
            seg = 0;
            all_dots = true;
            continue;
        }
        if ((unsigned char)c < 0x20 || c == 0x7f)
            return false;
        if (c != '.')
            all_dots = false;
        seg++;
        if (seg > FS_NAME_MAX)
            return false;
    }
    return true;
}

esp_err_t fsvol_resolve(const char *vpath, const fsvol_t **vol,
                        char *real, size_t cap)
{
    size_t id_len;
    if (!path_shape_ok(vpath, &id_len))
        return ESP_ERR_INVALID_ARG;

    char id[32];
    if (id_len >= sizeof id)
        return ESP_ERR_NOT_FOUND;   /* こんなに長い id は登録できない */
    memcpy(id, vpath + 1, id_len);
    id[id_len] = '\0';

    const fsvol_t *v = fsvol_find(id);
    if (!v)
        return ESP_ERR_NOT_FOUND;
    if (vol)
        *vol = v;

    lock();
    int idx = index_of(v);
    bool m = (idx >= 0) && mounted_locked(idx);
    unlock();
    if (!m)
        return ESP_ERR_INVALID_STATE;

    if (real && cap) {
        const char *rest = vpath + 1 + id_len;   /* "" か "/以下" */
        /* 末尾の '/' は落とす: "/sd/" -> "/sd"。POSIX の opendir は
           どちらでも通るが、stat/rename は実装差があるので揃える。 */
        size_t rlen = strlen(rest);
        while (rlen > 1 && rest[rlen - 1] == '/')
            rlen--;
        if (rlen == 1 && rest[0] == '/')
            rlen = 0;
        size_t rootlen = strlen(v->root);
        if (rootlen + rlen + 1 > cap)
            return ESP_ERR_INVALID_ARG;
        memcpy(real, v->root, rootlen);
        memcpy(real + rootlen, rest, rlen);
        real[rootlen + rlen] = '\0';
    }
    return ESP_OK;
}

const char *fs_err_str(esp_err_t err)
{
    switch (err) {
    case ESP_OK:                   return "ok";
    case ESP_ERR_INVALID_ARG:      return "bad path";
    case ESP_ERR_NOT_FOUND:        return "no such volume";
    case ESP_ERR_INVALID_STATE:    return "not mounted";
    case ESP_ERR_NOT_SUPPORTED:    return "not supported on this volume";
    case ESP_ERR_NO_MEM:           return "out of memory";
    case ESP_ERR_INVALID_SIZE:     return "too large";
    case ESP_ERR_NOT_FINISHED:     return "directory not empty";
    case ESP_FAIL:                 return "i/o error";
    default:                       return esp_err_to_name(err);
    }
}
