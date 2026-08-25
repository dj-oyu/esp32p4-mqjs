/*
 * ボリューム種別を問わないファイル操作。POSIX VFS の上に載るだけなので
 * LittleFS だろうと FAT だろうと同じコードが動く —— これが
 * docs/filer-storage-design.md §3 の「ボード差がコードから消える」の
 * 実体で、SD 固有の分岐はこのファイルに 1 つも無い。
 */
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "fs_core.h"

static const char *TAG = "fs_ops";

/* コピーの転送単位。SDMMC は PSRAM DMA 可 (SOC_SDMMC_PSRAM_DMA_CAPABLE)
   なので内蔵 SRAM を一切使わない。32 KB は FAT の 16 KB アロケーション
   ユニット 2 個分で、1 KB 刻みより往復が二桁減る。 */
#define COPY_CHUNK 32768

static void *ps_alloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    return p ? p : malloc(n);   /* PSRAM 無しのボードでも動くように */
}

/* errno を esp_err に畳む。JS に見せるのは fs_err_str() の短い文字列で、
   ここでは「どのエラーがどの文言になるか」だけ決める。 */
static esp_err_t from_errno(int e)
{
    switch (e) {
    case ENOENT:  return ESP_ERR_NOT_FOUND;
    case EACCES:
    case EPERM:   return ESP_ERR_INVALID_STATE;
    case EEXIST:  return ESP_ERR_INVALID_STATE;
    case ENOTEMPTY: return ESP_ERR_NOT_FINISHED;
    case ENOSPC:  return ESP_ERR_NO_MEM;
    case ENAMETOOLONG: return ESP_ERR_INVALID_ARG;
    default:      return ESP_FAIL;
    }
}

/* ---- ディレクトリのスナップショット ------------------------------ */

typedef struct {
    const char *name;
    bool        is_dir;
} fs_ent_t;

struct fs_dir {
    char     *pool;
    size_t    pool_len, pool_cap;
    fs_ent_t *ent;
    int       n, cap;
    bool      truncated;
    char      real[FS_PATH_MAX];   /* 実パス。fs_dir_stat が使う */
};

static bool pool_push(fs_dir_t *d, const char *s, size_t *off)
{
    size_t n = strlen(s) + 1;
    if (d->pool_len + n > d->pool_cap) {
        size_t cap = d->pool_cap ? d->pool_cap * 2 : 4096;
        while (cap < d->pool_len + n)
            cap *= 2;
        char *p = ps_alloc(cap);
        if (!p)
            return false;
        memcpy(p, d->pool, d->pool_len);
        free(d->pool);
        d->pool     = p;
        d->pool_cap = cap;
    }
    *off = d->pool_len;
    memcpy(d->pool + d->pool_len, s, n);
    d->pool_len += n;
    return true;
}

static int ent_cmp(const void *a, const void *b)
{
    const fs_ent_t *x = a, *y = b;
    if (x->is_dir != y->is_dir)
        return x->is_dir ? -1 : 1;      /* ディレクトリを先に */
    int c = strcasecmp(x->name, y->name);
    return c ? c : strcmp(x->name, y->name);
}

esp_err_t fs_dir_open(const char *vpath, fs_dir_t **out)
{
    *out = NULL;
    char real[FS_PATH_MAX];
    esp_err_t err = fsvol_resolve(vpath, NULL, real, sizeof real);
    if (err != ESP_OK)
        return err;

    DIR *dir = opendir(real);
    if (!dir)
        return from_errno(errno);

    fs_dir_t *d = ps_alloc(sizeof *d);
    if (!d) {
        closedir(dir);
        return ESP_ERR_NO_MEM;
    }
    memset(d, 0, sizeof *d);
    strlcpy(d->real, real, sizeof d->real);

    /* 1 パス目: 名前をプールに詰め、オフセットを控える。プールは
       realloc で動くので、ポインタ化は読み切ってから。 */
    size_t *offs = NULL;
    int     offs_cap = 0;
    struct dirent *e;
    while ((e = readdir(dir)) != NULL) {
        if (e->d_name[0] == '.' &&
            (e->d_name[1] == '\0' || (e->d_name[1] == '.' && e->d_name[2] == '\0')))
            continue;                    /* "." ".." は出さない */
        if (d->n >= FS_DIR_MAX) {
            d->truncated = true;
            break;
        }
        if (d->n >= offs_cap) {
            int    ncap = offs_cap ? offs_cap * 2 : 64;
            size_t *no  = ps_alloc((size_t)ncap * sizeof *no);
            fs_ent_t *ne = ps_alloc((size_t)ncap * sizeof *ne);
            if (!no || !ne) {
                free(no);
                free(ne);
                err = ESP_ERR_NO_MEM;
                break;
            }
            memcpy(no, offs, (size_t)d->n * sizeof *no);
            memcpy(ne, d->ent, (size_t)d->n * sizeof *ne);
            free(offs);
            free(d->ent);
            offs     = no;
            d->ent   = ne;
            offs_cap = ncap;
            d->cap   = ncap;
        }
        size_t off;
        if (!pool_push(d, e->d_name, &off)) {
            err = ESP_ERR_NO_MEM;
            break;
        }
        offs[d->n]         = off;
        d->ent[d->n].is_dir = (e->d_type == DT_DIR);
        d->n++;
    }
    closedir(dir);

    if (err != ESP_OK) {
        free(offs);
        fs_dir_close(d);
        return err;
    }

    for (int i = 0; i < d->n; i++)
        d->ent[i].name = d->pool + offs[i];
    free(offs);

    if (d->n > 1)
        qsort(d->ent, (size_t)d->n, sizeof *d->ent, ent_cmp);

    *out = d;
    return ESP_OK;
}

int  fs_dir_count(const fs_dir_t *d)     { return d ? d->n : 0; }
bool fs_dir_truncated(const fs_dir_t *d) { return d && d->truncated; }

bool fs_dir_get(const fs_dir_t *d, int i, fs_entry_t *out)
{
    if (!d || i < 0 || i >= d->n)
        return false;
    out->name   = d->ent[i].name;
    out->is_dir = d->ent[i].is_dir;
    return true;
}

esp_err_t fs_dir_stat(const fs_dir_t *d, int i, fs_stat_t *out)
{
    if (!d || i < 0 || i >= d->n)
        return ESP_ERR_INVALID_ARG;
    char path[FS_PATH_MAX];
    if ((size_t)snprintf(path, sizeof path, "%s/%s", d->real, d->ent[i].name)
            >= sizeof path)
        return ESP_ERR_INVALID_ARG;
    struct stat st;
    if (stat(path, &st) != 0)
        return from_errno(errno);
    out->is_dir = S_ISDIR(st.st_mode);
    out->size   = (uint64_t)st.st_size;
    out->mtime  = st.st_mtime;
    return ESP_OK;
}

void fs_dir_close(fs_dir_t *d)
{
    if (!d)
        return;
    free(d->pool);
    free(d->ent);
    free(d);
}

/* ---- 単体操作 ---------------------------------------------------- */

esp_err_t fs_stat(const char *vpath, fs_stat_t *out)
{
    char real[FS_PATH_MAX];
    esp_err_t err = fsvol_resolve(vpath, NULL, real, sizeof real);
    if (err != ESP_OK)
        return err;
    struct stat st;
    if (stat(real, &st) != 0)
        return from_errno(errno);
    out->is_dir = S_ISDIR(st.st_mode);
    out->size   = (uint64_t)st.st_size;
    out->mtime  = st.st_mtime;
    return ESP_OK;
}

esp_err_t fs_usage(const char *volume_id, uint64_t *total, uint64_t *freeb)
{
    const fsvol_t *v = fsvol_find(volume_id);
    if (!v)
        return ESP_ERR_NOT_FOUND;
    if (!fsvol_mounted(v))
        return ESP_ERR_INVALID_STATE;
    if (!v->ops || !v->ops->usage)
        return ESP_ERR_NOT_SUPPORTED;
    return v->ops->usage(v, total, freeb);
}

esp_err_t fs_read(const char *vpath, uint64_t off, void *buf, size_t cap,
                  size_t *n_read)
{
    *n_read = 0;
    char real[FS_PATH_MAX];
    esp_err_t err = fsvol_resolve(vpath, NULL, real, sizeof real);
    if (err != ESP_OK)
        return err;

    FILE *f = fopen(real, "rb");
    if (!f)
        return from_errno(errno);
    if (off && fseek(f, (long)off, SEEK_SET) != 0) {
        fclose(f);
        return ESP_OK;              /* 末尾より後ろ = 0 バイト読めた */
    }
    *n_read = fread(buf, 1, cap, f);
    bool bad = ferror(f);
    fclose(f);
    return bad ? ESP_FAIL : ESP_OK;
}

esp_err_t fs_write(const char *vpath, const void *buf, size_t len, bool append)
{
    char real[FS_PATH_MAX];
    esp_err_t err = fsvol_resolve(vpath, NULL, real, sizeof real);
    if (err != ESP_OK)
        return err;

    FILE *f = fopen(real, append ? "ab" : "wb");
    if (!f)
        return from_errno(errno);
    size_t wr = fwrite(buf, 1, len, f);
    /* 抜き挿し検出ピンが無いので (§6)、書いたら必ず閉じ切る。ここで
       fsync まで通しておけば、失うのは書きかけの 1 ファイルだけ。 */
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    if (wr != len) {
        ESP_LOGE(TAG, "short write %zu/%zu on %s", wr, len, real);
        return ESP_ERR_NO_MEM;      /* 実質いつも ENOSPC */
    }
    return ESP_OK;
}

esp_err_t fs_mkdir(const char *vpath)
{
    char real[FS_PATH_MAX];
    esp_err_t err = fsvol_resolve(vpath, NULL, real, sizeof real);
    if (err != ESP_OK)
        return err;
    if (mkdir(real, 0777) != 0)
        return from_errno(errno);
    return ESP_OK;
}

/* 実パスに対する再帰削除。仮想パスの検査は呼び出し元で済んでいる。 */
static esp_err_t rm_real(const char *real, bool recursive, int depth)
{
    struct stat st;
    if (stat(real, &st) != 0)
        return from_errno(errno);

    if (!S_ISDIR(st.st_mode))
        return unlink(real) == 0 ? ESP_OK : from_errno(errno);

    if (recursive) {
        if (depth >= FS_RM_DEPTH_MAX)
            return ESP_ERR_INVALID_SIZE;
        DIR *dir = opendir(real);
        if (!dir)
            return from_errno(errno);
        char *child = malloc(FS_PATH_MAX);
        if (!child) {
            closedir(dir);
            return ESP_ERR_NO_MEM;
        }
        esp_err_t err = ESP_OK;
        struct dirent *e;
        while (err == ESP_OK && (e = readdir(dir)) != NULL) {
            if (e->d_name[0] == '.' &&
                (e->d_name[1] == '\0' ||
                 (e->d_name[1] == '.' && e->d_name[2] == '\0')))
                continue;
            if ((size_t)snprintf(child, FS_PATH_MAX, "%s/%s", real, e->d_name)
                    >= FS_PATH_MAX) {
                err = ESP_ERR_INVALID_ARG;
                break;
            }
            err = rm_real(child, true, depth + 1);
        }
        free(child);
        closedir(dir);
        if (err != ESP_OK)
            return err;
    }
    return rmdir(real) == 0 ? ESP_OK : from_errno(errno);
}

esp_err_t fs_remove(const char *vpath, bool recursive)
{
    char real[FS_PATH_MAX];
    const fsvol_t *v;
    esp_err_t err = fsvol_resolve(vpath, &v, real, sizeof real);
    if (err != ESP_OK)
        return err;
    /* ボリュームそのもの ("/sd") を消させない。 */
    if (strcmp(real, v->root) == 0)
        return ESP_ERR_INVALID_ARG;
    return rm_real(real, recursive, 0);
}

esp_err_t fs_copy(const char *from_vpath, const char *to_vpath,
                  void (*progress)(void *ctx, uint64_t done, uint64_t total),
                  void *ctx)
{
    char from[FS_PATH_MAX], to[FS_PATH_MAX];
    esp_err_t err = fsvol_resolve(from_vpath, NULL, from, sizeof from);
    if (err != ESP_OK)
        return err;
    err = fsvol_resolve(to_vpath, NULL, to, sizeof to);
    if (err != ESP_OK)
        return err;
    if (strcmp(from, to) == 0)
        return ESP_ERR_INVALID_ARG;

    struct stat st;
    if (stat(from, &st) != 0)
        return from_errno(errno);
    if (S_ISDIR(st.st_mode))
        return ESP_ERR_NOT_SUPPORTED;   /* ディレクトリのコピーは JS 側で
                                           1 ファイルずつ (進捗を出せる) */

    FILE *in = fopen(from, "rb");
    if (!in)
        return from_errno(errno);
    FILE *out = fopen(to, "wb");
    if (!out) {
        fclose(in);
        return from_errno(errno);
    }
    char *buf = ps_alloc(COPY_CHUNK);
    if (!buf) {
        fclose(in);
        fclose(out);
        unlink(to);
        return ESP_ERR_NO_MEM;
    }

    uint64_t done = 0;
    err = ESP_OK;
    for (;;) {
        size_t n = fread(buf, 1, COPY_CHUNK, in);
        if (n == 0) {
            if (ferror(in))
                err = ESP_FAIL;
            break;
        }
        if (fwrite(buf, 1, n, out) != n) {
            err = ESP_ERR_NO_MEM;       /* まず ENOSPC */
            break;
        }
        done += n;
        if (progress)
            progress(ctx, done, (uint64_t)st.st_size);
    }
    free(buf);
    fflush(out);
    fsync(fileno(out));
    fclose(out);
    fclose(in);
    if (err != ESP_OK)
        unlink(to);                     /* 半端なコピーを残さない */
    return err;
}

esp_err_t fs_move(const char *from_vpath, const char *to_vpath)
{
    char from[FS_PATH_MAX], to[FS_PATH_MAX];
    const fsvol_t *vf, *vt;
    esp_err_t err = fsvol_resolve(from_vpath, &vf, from, sizeof from);
    if (err != ESP_OK)
        return err;
    err = fsvol_resolve(to_vpath, &vt, to, sizeof to);
    if (err != ESP_OK)
        return err;

    if (vf == vt)
        return rename(from, to) == 0 ? ESP_OK : from_errno(errno);

    /* ボリュームをまたぐと rename(2) は使えない。コピーしてから消す。
       コピーが失敗したら元は消さない —— 中途半端な移動で原本を失う方が
       「移動できませんでした」より遥かに悪い。 */
    err = fs_copy(from_vpath, to_vpath, NULL, NULL);
    if (err != ESP_OK)
        return err;
    return fs_remove(from_vpath, false);
}
