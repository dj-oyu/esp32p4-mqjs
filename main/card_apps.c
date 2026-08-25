/*
 * microSD 上のアプリを、棚 (MQTT) と同じカタログ契約で見せる。
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
 * そのまま置き、**インストールする瞬間に** crypto_sign_open で検証する。
 * 新しい暗号は 1 行も要らず、鍵の持ち主だけが配れるという性質が保たれる。
 * 作るのは tools/mqjs_pack.py。
 *
 * 検証はインストールにしかない。一覧は信頼できる必要がないからで、
 * 逆に一覧で検証すると (実機 226ms/本) ストア画面を開くたびに JS タスクが
 * 秒単位で止まる。カタログ行は「カードにこういう名前のものが在る」以上の
 * ことを主張していない。
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
/* マニフェスト行が始まるオフセット。カード上の形は signature(64) || script
   なので、署名を読み飛ばした先がそのままスクリプトの先頭
   ("// @title ...") になる。 */
#define MANIFEST_OFF SIG_LEN
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
   スクリプト本体 (NUL 終端)、*len に長さ。検証に落ちたら NULL で、
   *why に短い理由が入る (why は NULL 可)。
 *
 * **実機で 1 本 226ms**。crypto_sign_open は Ed25519 のソフト実装で、
 * JS タスク (協調・単一スレッド) の上で回ると UI ごと止まる。だから
 * ここを呼ぶのはインストールの瞬間だけ —— 人が押したボタン 1 回に
 * 対して 1 回で、それは待たされてよい時間。一覧では呼ばない。 */
static char *load_verified(const char *name, size_t *len, const char **why)
{
    *len = 0;
    if (why)
        *why = "unknown";
    char vpath[128];
    snprintf(vpath, sizeof vpath, CARD_DIR "/%s" CARD_EXT, name);

    fs_stat_t st;
    if (fs_stat(vpath, &st) != ESP_OK || st.is_dir) {
        if (why)
            *why = "not on the card any more";
        return NULL;
    }
    if (st.size <= SIG_LEN || st.size > SIG_LEN + MQJS_SCRIPT_MAX) {
        ESP_LOGW(TAG, "'%s': implausible size %llu", name, st.size);
        if (why)
            *why = "implausible size";
        return NULL;
    }

    size_t n = (size_t)st.size;
    unsigned char *sm = malloc(n);
    /* crypto_sign_open は n バイトぶんの出力先を要求する */
    unsigned char *m = malloc(n + 1);
    if (!sm || !m) {
        free(sm);
        free(m);
        if (why)
            *why = "out of memory";
        return NULL;
    }
    size_t got = 0;
    if (fs_read(vpath, 0, sm, n, &got) != ESP_OK || got != n) {
        free(sm);
        free(m);
        if (why)
            *why = "cannot read the card";
        return NULL;
    }
    unsigned long long mlen = 0;
    int rc = crypto_sign_open(m, &mlen, sm, n, MQJS_TASK_PUBKEY);
    free(sm);
    if (rc != 0) {
        /* 署名が合わない = 鍵の持ち主が配ったものではない。一覧には
           出したうえで、ここで断る (以前は一覧から黙って消していたので、
           カードに置いたのに出てこない理由が誰にも分からなかった)。 */
        ESP_LOGW(TAG, "'%s': signature rejected", name);
        if (why)
            *why = "signature rejected";
        free(m);
        return NULL;
    }
    m[mlen] = '\0';
    *len = (size_t)mlen;
    return (char *)m;
}

/* ---- カタログ契約 (mqjs_store_api_t) ---------------------------- */

/* 一覧は**署名を確かめない**。
 *
 * 以前はここで 1 本ずつ load_verified() を回していた。実機の実測で
 * 1 本 226ms、カードに 8 本で 1.8 秒、20 本なら 4.5 秒 —— しかも
 * ストア画面を開くたび。JS タスクは協調・単一スレッドなので、その間
 * UI も他のアプリも全部止まる。
 *
 * 一覧に信頼性は要らない。要るのはインストールの側だけで、そちらは
 * card_install() が crypto_sign_open で確かめて落ちれば断る。カタログ行が
 * 名乗る @title は所詮 自称であって、それを信じて何かが起きるわけではない
 * (押して初めて検証が走る)。だから読むのはマニフェスト行だけ: 署名 64 バイト
 * を飛ばした先から HEAD_MAX だけ。全文を読むことも malloc することもない。
 *
 * 副作用として、署名が合わないファイルも**一覧に出る**ようになった。これは
 * 改善で、以前はログにしか出ないまま一覧から消えていたので、カードに置いた
 * のに出てこない理由が画面からは分からなかった。行には verified が付かない
 * (sys.store() 参照) ので、UI は「まだ確かめていない」と正しく言える。
 *
 * 残る一覧時の門は split_name() のファイル名検査だけ。安いし、名前はその
 * まま /littlefs/apps/<name>.js になるので依然として必要。 */
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

        char vpath[128];
        snprintf(vpath, sizeof vpath, CARD_DIR "/%s" CARD_EXT, name);
        fs_stat_t st;
        if (fs_stat(vpath, &st) != ESP_OK || st.is_dir)
            continue;
        /* 大きさの妥当性だけは一覧でも見る。署名しか入っていない
           ファイルや、そもそも読み込めない大きさのものを行にしても
           押した先で必ず断られるだけ。 */
        if (st.size <= SIG_LEN || st.size > SIG_LEN + MQJS_SCRIPT_MAX) {
            ESP_LOGW(TAG, "'%s': implausible size %llu", name, st.size);
            continue;
        }

        char  *head = s_cat[s_cat_n].head;
        size_t got  = 0;
        if (fs_read(vpath, MANIFEST_OFF, head, HEAD_MAX - 1, &got) != ESP_OK)
            continue;
        head[got] = '\0';
        /* 検証前のバイト列が初めてカタログ行になるので、制御文字だけは
           落とす。'\n' は残す (マニフェストの行境界)。0x80 以上は UTF-8
           の続きなので触らない —— @title は日本語で書かれる。
           署名の合わないファイルがステータスバーへ ESC を撃ち込めない
           ようにするためだけの、行儀の悪いバイトへの目張り。 */
        for (size_t k = 0; k < got; k++) {
            unsigned char c = (unsigned char)head[k];
            if ((c < 0x20 && c != '\n') || c == 0x7f)
                head[k] = ' ';
        }

        snprintf(s_cat[s_cat_n].name, sizeof s_cat[s_cat_n].name, "%s", name);
        s_cat[s_cat_n].size = (long)(st.size - SIG_LEN);
        s_cat_n++;
    }
    fs_dir_close(d);
    if (s_cat_n)
        ESP_LOGI(TAG, "%d app(s) on the card (signatures unchecked)", s_cat_n);
    return s_cat_n;
}

static bool card_get(int idx, char *name, size_t ncap, char *head, size_t hcap)
{
    if (idx < 0 || idx >= s_cat_n)
        return false;
    snprintf(name, ncap, "%s", s_cat[idx].name);
    /* カタログ行の @size は本体の実長 (ファイル長 - 署名 64)。棚側と
       同じ形にしておくとランチャーが出所を意識せず同じコードで表示できる。
     *
     * @size を**先に**置くのは、マニフェストが hcap を埋め尽くしたときに
     * 切り落とされるのが末尾だから。後ろに付けると、行の多いアプリだけ
     * サイズが 0 と表示される。@ 行の順序は読む側 (manifest_field) には
     * 関係ない。 */
    snprintf(head, hcap, "// @size %ld\n%s", s_cat[idx].size,
             s_cat[idx].head);
    return true;
}

/* 棚側の install は非同期 (ブローカーへ取りに行く) だが、こちらは
   目の前のカードから読むだけなのでその場で終わる。戻り値も
   「要求を受け付けた」ではなく「入った」になる。
 *
 * **署名検証はここにしかない。** 一覧は誰でも書けるカードのバイト列を
 * そのまま並べているだけなので、/littlefs/apps/ の不変条件を守っている
 * のはこの crypto_sign_open ただ 1 つ。226ms かかるが、人がボタンを
 * 押した 1 回に対する 1 回なので払ってよい。 */
static bool card_install(const char *name)
{
    size_t len = 0;
    const char *why = NULL;
    char *body = load_verified(name, &len, &why);
    if (!body) {
        /* 断った理由を残す。一覧に出ているのに入らないアプリが在りうる
           ようになったので、「読めません」だけでは人が次に何をすれば
           いいか分からない。 */
        ESP_LOGE(TAG, "refusing to install '%s': %s", name, why);
        return false;
    }
    bool ok = storage_save_app(name, body, len);
    free(body);
    if (ok)
        ESP_LOGI(TAG, "installed '%s' from the card (signature verified)", name);
    else
        ESP_LOGE(TAG, "refusing to install '%s': cannot write to internal storage",
                 name);
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
