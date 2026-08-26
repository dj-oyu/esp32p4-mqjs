/*
 * fs_picker の純粋な小道具をホストで叩く。
 *
 * **テスト対象は fs_picker.cpp のテキストそのもの**: run_tests.sh が
 * "host-testable helpers begin/end" マーカーの間を切り出して
 * build/helpers.inc に置き、それをここが include する。書き写した
 * コピーを検査して「本体は直っていないのに緑」になるのを避けるため。
 *
 * ここで見ていないもの (device でしか確かめられない):
 *   - LVGL のオブジェクト構築、タッチ、モーダルの重なり
 *   - fs_dir_open の所要時間 (spec §F #7)
 *   - lv_timer の駆動と 1 フレームあたりの行数 (FS_PICK_ROWS_PER_TICK)
 *   - cb が UI タスクで呼ばれること
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "fs_grant.h" /* FS_OP_* */

/* 本体が fs_core.h / fs_picker.h から得ている値。ここでは IDF ヘッダを
   引けないので同じ数字を置く。ずれたら下の _Static_assert 相当が無い分
   静かに通ってしまうので、値の出所をコメントで固定しておく。 */
#define FS_NAME_MAX      256 /* fs_core.h */
#define FS_PICK_EXTS_MAX 6   /* fs_picker.h */
#define FS_PICK_EXT_MAX  12  /* fs_picker.h */
/* fs_picker.h の定義そのもの (向こうも FS_GRANT_ROOT_MAX を書いている)。
   数値を写していないので、器が動いてもこのテストは一緒に動く。 */
#define FS_PICK_SCOPE_MAX FS_GRANT_ROOT_MAX

/* ext_ok が触るフィールドだけの Pick。本体の struct Pick は LVGL/IDF 型を
   含むのでそのままは持ち込めない。 */
struct Pick {
    int  n_exts;
    char exts[FS_PICK_EXTS_MAX][FS_PICK_EXT_MAX];
};

#include "helpers.inc"

static int fails;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) {                                                           \
            printf("FAIL %d: %s\n", __LINE__, #c);                            \
            fails++;                                                          \
        }                                                                     \
    } while (0)
#define EQS(a, b)                                                             \
    do {                                                                      \
        if (strcmp((a), (b))) {                                               \
            printf("FAIL %d: \"%s\" != \"%s\"\n", __LINE__, (a), (b));        \
            fails++;                                                          \
        }                                                                     \
    } while (0)

int main(void)
{
    char b[64];

    /* ---- join_path -----------------------------------------------
       "//" は fs_vol.c の path_shape_ok が拒む形なので、ここで作ると
       「一覧には出るのに開けない」パスが生まれる。 */
    join_path(b, sizeof b, "/internal/scripts", "a.js");
    EQS(b, "/internal/scripts/a.js");
    join_path(b, sizeof b, "/internal/", "a.js");
    EQS(b, "/internal/a.js");
    join_path(b, sizeof b, "/internal///", "a.js");
    EQS(b, "/internal/a.js");
    join_path(b, sizeof b, "/", "a.js");
    EQS(b, "/a.js");
    join_path(b, sizeof b, "", "a.js");
    EQS(b, "/a.js");

    /* ---- parent_path ----------------------------------------------
       ボリューム直下の親は "" (= ボリューム一覧)。ここを間違えると
       「上へ」で抜けられない階層ができる。 */
    strcpy(b, "/internal/scripts/x");
    parent_path(b);
    EQS(b, "/internal/scripts");
    strcpy(b, "/internal/scripts");
    parent_path(b);
    EQS(b, "/internal");
    strcpy(b, "/internal");
    parent_path(b);
    EQS(b, "");
    strcpy(b, "/internal/");
    parent_path(b);
    EQS(b, "");
    strcpy(b, "/");
    parent_path(b);
    EQS(b, "");
    strcpy(b, "");
    parent_path(b);
    EQS(b, "");

    /* ---- copy_str: 器を超えたら切り捨て、必ず終端 ---- */
    char s8[8];
    copy_str(s8, sizeof s8, "0123456789");
    EQS(s8, "0123456");
    copy_str(s8, sizeof s8, NULL);
    EQS(s8, "");
    copy_str(s8, sizeof s8, "ab");
    EQS(s8, "ab");

    /* ---- copy_line: 制御文字は空白へ潰す (同意画面の reason) ----
       改行をそのまま通すと、アプリが reason に改行を詰めるだけで
       「許可 / 拒否」を画面外へ押し出せる。UTF-8 の後続バイト
       (>= 0x80) には触らないこと。 */
    {
        char l[32];
        copy_line(l, sizeof l, "a\nb\tc\x7f" "d");
        EQS(l, "a b c d");
        copy_line(l, sizeof l, NULL);
        EQS(l, "");
        copy_line(l, sizeof l, "設定を読みます");
        EQS(l, "設定を読みます");
    }

    /* ---- fits_scope: grant の器に入らない道は選ばせない (S6) ----
       ここに落ちる道を返すと runtime は黙って ok=false に倒す =
       「タップしたのにキャンセル扱い」になる。 */
    {
        char path[FS_PICK_SCOPE_MAX + 8];
        CHECK(!fits_scope(NULL));
        CHECK(!fits_scope(""));
        CHECK(fits_scope("/sd/a.txt"));
        /* ちょうど器に収まる最大長 = FS_PICK_SCOPE_MAX - 1 バイト */
        memset(path, 'x', FS_PICK_SCOPE_MAX - 1);
        path[FS_PICK_SCOPE_MAX - 1] = '\0';
        CHECK(fits_scope(path));
        /* 1 バイト増えると runtime の snprintf があふれる */
        memset(path, 'x', FS_PICK_SCOPE_MAX);
        path[FS_PICK_SCOPE_MAX] = '\0';
        CHECK(!fits_scope(path));
    }

    /* ---- ext_ok: 後方一致・大小無視 ---- */
    struct Pick p;
    memset(&p, 0, sizeof p);
    CHECK(ext_ok(&p, "anything")); /* フィルタ無し = 全部通す */
    p.n_exts = 1;
    strcpy(p.exts[0], ".js");
    CHECK(ext_ok(&p, "a.js"));
    CHECK(ext_ok(&p, "A.JS"));
    CHECK(!ext_ok(&p, "a.json")); /* 前方一致に倒れていないこと */
    CHECK(!ext_ok(&p, "js"));     /* 拡張子より短い名前で読み越さない */
    CHECK(ext_ok(&p, ".js"));     /* 名前ちょうど */
    p.n_exts = 2;
    strcpy(p.exts[1], ".txt");
    CHECK(ext_ok(&p, "readme.txt"));
    CHECK(!ext_ok(&p, "readme.md"));

    /* ---- name_ok: 保存名として fs_vol.c を通る形か ---- */
    CHECK(name_ok("a.js"));
    CHECK(name_ok(".hidden"));
    CHECK(!name_ok(""));
    CHECK(!name_ok(NULL));
    CHECK(!name_ok("."));
    CHECK(!name_ok(".."));
    CHECK(!name_ok("...")); /* path_shape_ok は「全部ドット」を拒む */
    CHECK(!name_ok("a/b"));
    CHECK(!name_ok("a\tb"));
    CHECK(!name_ok("a\x7f"));
    {
        char big[FS_NAME_MAX + 8];
        memset(big, 'x', sizeof big - 1);
        big[sizeof big - 1] = '\0';
        CHECK(!name_ok(big));
    }

    /* ---- ops_text: 同意画面が「何を求められたか」を落とさない ---- */
    char o[128];
    ops_text(FS_OP_READ, o, sizeof o);
    EQS(o, "読み取り");
    ops_text(FS_OP_READ | FS_OP_WRITE, o, sizeof o);
    EQS(o, "読み取り / 書き込み");
    ops_text(0, o, sizeof o);
    EQS(o, "(操作の指定なし)");
    ops_text(FS_OP_ALL, o, sizeof o);
    EQS(o, "読み取り / 書き込み / 作成 / 削除 / 名前変更");
    /* 未知のビットを黙って落とすと、実際より狭い許可に見えるまま
       ユーザが「許可」を押す。それは検出できないので必ず出す。 */
    ops_text(0x80, o, sizeof o);
    EQS(o, "不明な操作");
    ops_text(FS_OP_READ | 0x40, o, sizeof o);
    EQS(o, "読み取り / 不明な操作");
    {
        char tiny[10];
        ops_text(FS_OP_ALL, tiny, sizeof tiny);
        CHECK(strlen(tiny) < sizeof tiny); /* あふれない・必ず終端 */
    }

    if (fails)
        printf("FAILED (%d)\n", fails);
    else
        printf("ok\n");
    return fails ? 1 : 0;
}
