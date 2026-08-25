/*
 * microSD 上の署名済みアプリを、棚 (MQTT) と同じカタログ契約で見せる。
 * docs/filer-storage-design.md §13。
 *
 * ねらいは「ブローカー無しで配れること」であって、容量ではない
 * (littlefs 1MB に JS アプリは 70 本以上入る)。だから **アプリの住処は
 * littlefs のまま**で、カードは第 3 の「入手可能」ソースとして並ぶだけ。
 * こうするとカードを抜いても autostart アプリが消えず、起動がカードの
 * 有無に依存しない。
 *
 * 信頼モデルは一切変えない。littlefs の apps/ が無検証で読めるのは
 * そこへ届く経路が署名検証済みしか無いからで (main/storage.c 冒頭)、
 * カードは誰でも PC で書ける。そこで **カード上のファイルは MQTT に
 * publish されるのと同じバイト列** —— signature(64) || script —— を
 * そのまま置き、読むたびに crypto_sign_open で検証する。新しい暗号は
 * 1 行も要らず、鍵の持ち主だけが配れるという性質が保たれる。
 * 作るのは tools/mqjs_pack.py。
 *
 * 走査はカタログの count() の中でだけ走る。ストア画面を開いたときにしか
 * 呼ばれないので、常駐タスクもタイマーも要らない (電源方針)。
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "fs_core.h"
#include "mqjs_runtime.h"
#include "storage.h"
#include "tweetnacl.h"

#include "card_apps.h"
#include "task_pubkey.h"

#define CARD_DIR   "/sd/apps"
#define CARD_EXT   ".mjsa"
#define SIG_LEN    64
/* 1 回の走査で覚えるアプリ数。カタログ 1 画面ぶんあれば十分で、
   これ以上は人が選べない。 */
#define CARD_MAX   32
/* マニフェスト行だけ読めればカタログは作れる。本体はインストール時に
   改めて読む —— 走査のたびに全文を読むと、カードが遅いときに
   ストア画面が固まる。 */
#define HEAD_MAX   224

static const char *TAG = "card_apps";

static struct {
    char name[25];
    char head[HEAD_MAX];
    long size;                  /* 署名を除いたスクリプト長 */
} s_cat[CARD_MAX];
static int s_cat_n;

/* "<name>.mjsa" -> name。拡張子が違えば false。 */
static bool split_name(const char *fname, char *out, size_t cap)
{
    size_t n = strlen(fname);
    size_t e = strlen(CARD_EXT);
    if (n <= e || strcmp(fname + n - e, CARD_EXT))
        return false;
    size_t base = n - e;
    if (base == 0 || base >= cap)
        return false;
    /* 名前はそのままファイルパスとアプリ名になる。'/' や '.' を含む
       ものはここで落とす (storage_save_app は sanitize 済みを期待する)。 */
    for (size_t i = 0; i < base; i++) {
        char c = fname[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok)
            return false;
    }
    memcpy(out, fname, base);
    out[base] = '\0';
    return true;
}

/* 署名付きの塊を読み、検証して、中身を返す。戻り値は malloc'd な
   スクリプト本体 (NUL 終端)、*len に長さ。検証に落ちたら NULL。 */
static char *load_verified(const char *name, size_t *len)
{
    *len = 0;
    char vpath[128];
    snprintf(vpath, sizeof vpath, CARD_DIR "/%s" CARD_EXT, name);

    fs_stat_t st;
    if (fs_stat(vpath, &st) != ESP_OK || st.is_dir)
        return NULL;
    if (st.size <= SIG_LEN || st.size > SIG_LEN + MQJS_SCRIPT_MAX) {
        ESP_LOGW(TAG, "'%s': implausible size %llu", name, st.size);
        return NULL;
    }

    size_t n = (size_t)st.size;
    unsigned char *sm = malloc(n);
    /* crypto_sign_open は n バイトぶんの出力先を要求する */
    unsigned char *m = malloc(n + 1);
    if (!sm || !m) {
        free(sm);
        free(m);
        return NULL;
    }
    size_t got = 0;
    if (fs_read(vpath, 0, sm, n, &got) != ESP_OK || got != n) {
        free(sm);
        free(m);
        return NULL;
    }
    unsigned long long mlen = 0;
    int rc = crypto_sign_open(m, &mlen, sm, n, MQJS_TASK_PUBKEY);
    free(sm);
    if (rc != 0) {
        /* 署名が合わない = 鍵の持ち主が配ったものではない。黙って無視
           するのではなくログに出す: カードに置いたのに出てこない理由が
           分からないと、人は「壊れている」と思ってしまう。 */
        ESP_LOGW(TAG, "'%s': signature rejected", name);
        free(m);
        return NULL;
    }
    m[mlen] = '\0';
    *len = (size_t)mlen;
    return (char *)m;
}

/* ---- カタログ契約 (mqjs_store_api_t) ---------------------------- */

static int card_count(void)
{
    s_cat_n = 0;
    /* ボリュームが無い (Stamp) / カードが入っていない / apps/ が無い、は
       すべて fs_dir_open の失敗として同じ「0 本」に落ちる。呼び出し側に
       ボードの分岐は要らない。 */
    fs_dir_t *d = NULL;
    if (fs_dir_open(CARD_DIR, &d) != ESP_OK)
        return 0;

    int total = fs_dir_count(d);
    for (int i = 0; i < total && s_cat_n < CARD_MAX; i++) {
        fs_entry_t e;
        if (!fs_dir_get(d, i, &e) || e.is_dir)
            continue;
        char name[25];
        if (!split_name(e.name, name, sizeof name))
            continue;
        size_t len = 0;
        char *body = load_verified(name, &len);
        if (!body)
            continue;               /* 署名なし / 壊れている */
        snprintf(s_cat[s_cat_n].name, sizeof s_cat[s_cat_n].name, "%s", name);
        size_t hn = len < HEAD_MAX - 1 ? len : HEAD_MAX - 1;
        memcpy(s_cat[s_cat_n].head, body, hn);
        s_cat[s_cat_n].head[hn] = '\0';
        s_cat[s_cat_n].size = (long)len;
        s_cat_n++;
        free(body);
    }
    fs_dir_close(d);
    if (s_cat_n)
        ESP_LOGI(TAG, "%d signed app(s) on the card", s_cat_n);
    return s_cat_n;
}

static bool card_get(int idx, char *name, size_t ncap, char *head, size_t hcap)
{
    if (idx < 0 || idx >= s_cat_n)
        return false;
    snprintf(name, ncap, "%s", s_cat[idx].name);
    /* カタログ行の @size は本体の実長。棚側と同じ形にしておくと
       ランチャーが出所を意識せず同じコードで表示できる。 */
    snprintf(head, hcap, "%s\n// @size %ld\n", s_cat[idx].head,
             s_cat[idx].size);
    return true;
}

/* 棚側の install は非同期 (ブローカーへ取りに行く) だが、こちらは
   目の前のカードから読むだけなのでその場で終わる。戻り値も
   「要求を受け付けた」ではなく「入った」になる。 */
static bool card_install(const char *name)
{
    size_t len = 0;
    char *body = load_verified(name, &len);
    if (!body)
        return false;
    bool ok = storage_save_app(name, body, len);
    free(body);
    if (ok)
        ESP_LOGI(TAG, "installed '%s' from the card", name);
    return ok;
}

static const mqjs_store_api_t s_card_api = {
    .count   = card_count,
    .get     = card_get,
    .install = card_install,
};

void card_apps_init(void)
{
    mqjs_set_card_provider(&s_card_api);
}
