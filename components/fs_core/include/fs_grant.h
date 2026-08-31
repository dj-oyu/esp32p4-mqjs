/*
 * fs_grant.h — grant (どこまで・何をしてよいか) の判定だけを持つ純 C99。
 *
 * 権限「表」はここには無い。表を持ち、トークンを発行し、worker/gen/epoch で
 * 寿命を見るのは今までどおり mqjs_runtime.c (JS タスク 1 本だけが触るので
 * ロックが要らない、という既存の設計を崩さない)。ここに切り出したのは
 * **判定**だけ ── 範囲の一致、操作ビットの包含、暗黙 grant、そして予約
 * サブツリーとの合成。
 *
 * 切り出す理由は 1 つで、この判定は静かに間違えるから。
 * "/internal/apps" と "/internal/appsfoo" を取り違えても、他アプリの
 * ディレクトリへ越境しても、動いているように見える。fs_reserved.c を
 * 純 C にしたのと同じ手で、ホストの gcc で直接叩けるようにする
 * (pc_test/test_grant_scope.c)。ESP-IDF のヘッダは 1 本も含めない。
 * malloc もしない (呼び出し元は JS タスクと UI タスクの両方)。
 *
 * 対応する仕様: docs/native-editor-spec.md §A.5、
 *               docs/native-editor-design.md §4.2 §4.3 §4.6。
 */
#ifndef FS_GRANT_H
#define FS_GRANT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 範囲の種別。SUBTREE が既存の grant の意味 (前置一致)。 */
enum {
    FS_SCOPE_SUBTREE = 0,   /* root と、その下すべて */
    FS_SCOPE_FILE    = 1,   /* root ただ 1 つ。完全一致 */
    FS_SCOPE_APPDIR  = 2,   /* アプリ私有ディレクトリ。一致規則は SUBTREE と同じ */
};

/* 操作。既存の bool write を置き換える (spec §A.5)。 */
enum {
    FS_OP_READ   = 1,
    FS_OP_WRITE  = 2,
    FS_OP_CREATE = 4,
    FS_OP_DELETE = 8,
    FS_OP_RENAME = 16,
};

#define FS_OP_ALL    (FS_OP_READ | FS_OP_WRITE | FS_OP_CREATE | \
                      FS_OP_DELETE | FS_OP_RENAME)
/* 予約サブツリーが拒む操作。READ は入らない ── fs_core.h が明言している
   とおり、予約するのは変更だけで、アプリのソースは秘密ではない。
   ここに FS_OP_READ を足すとランチャーが /internal/apps を一覧できなくなる。 */
#define FS_OP_MUTATE (FS_OP_WRITE | FS_OP_CREATE | FS_OP_DELETE | FS_OP_RENAME)

/* root の器。mqjs_runtime.c の MQJS_FS_SCOPE_MAX と同じ値でなければ
   ならない (FsGrant がこの構造体を埋め込む前提)。数値を 2 箇所に置くのは
   本意ではないが、runtime 側のヘッダを fs_core が引くわけにはいかない。
   ずれたときに黙って通ることはない: root が器の中で終端していなければ
   fs_grant_path_ok が false を返し、判定は「拒否」に倒れる。 */
#define FS_GRANT_ROOT_MAX 128

/* fs_core.h の FS_PATH_MAX / FS_NAME_MAX の写し。ESP 側で両方が見えている
   翻訳単位では食い違いをコンパイル時に落とす。 */
#define FS_GRANT_PATH_MAX 512
#define FS_GRANT_NAME_MAX 256
#if defined(FS_PATH_MAX) && defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(FS_GRANT_PATH_MAX == FS_PATH_MAX, "FS_GRANT_PATH_MAX != FS_PATH_MAX");
_Static_assert(FS_GRANT_NAME_MAX == FS_NAME_MAX, "FS_GRANT_NAME_MAX != FS_NAME_MAX");
#endif

/* 判定に要る分だけ。token/worker/gen/epoch/vol は runtime の持ち物で、
   ここからは見えない (見えたら寿命の判定がここにも生えてしまう)。 */
typedef struct {
    uint8_t kind;                   /* FS_SCOPE_* */
    uint8_t ops;                    /* FS_OP_* の or */
    char    root[FS_GRANT_ROOT_MAX];
} fs_grant_scope_t;

/* 拒否の理由。呼び出し元の例外文言を分けるためと、テストが「なぜ落ちたか」
   まで固定できるようにするため (通った/落ちたの 2 値だと、範囲で落ちたのか
   操作で落ちたのかを取り違えたまま緑になる)。 */
typedef enum {
    FS_GRANT_OK = 0,
    FS_GRANT_DENY_RESERVED,   /* 予約サブツリー。grant の内容によらず変更不可 */
    FS_GRANT_DENY_SCOPE,      /* grant はあるが範囲の外 */
    FS_GRANT_DENY_OPS,        /* 範囲内だが、その操作は許されていない */
    FS_GRANT_DENY_NO_GRANT,   /* grant が無く、暗黙 grant にも当たらない */
    FS_GRANT_DENY_BADPATH,    /* パスの形が不正 (NULL・相対・".."・"//"・制御文字・長すぎ) */
    FS_GRANT_DENY_BADREQ,     /* need_ops が 0 か、未定義のビットを含む */
} fs_grant_verdict_t;

/* vpath が仮想パスの形をしているか。fs_vol.c の path_shape_ok と同じ規則
   (先頭 '/'、ボリューム id が非空、"//" 不可、"." ".." 等の全ドット
   セグメント不可、制御文字不可、末尾の '/' は 1 個だけ可)。
 *
 * 判定を resolve より緩くしないために、ここで独立に見る。バインディングは
 * grant を見てから fs_ops を呼ぶので、判定の時点ではまだ誰もパスの形を
 * 検査していない。 */
bool fs_grant_path_ok(const char *vpath);

/* app が暗黙 grant の <app> として使える形か。'/' を含まない、全ドットでない、
   制御文字を含まない、1..FS_GRANT_NAME_MAX 文字。 */
bool fs_grant_name_ok(const char *app);

/* have が need をすべて含むか。need が 0 なら false (何を要求しているか
   言わない呼び出しは通さない)。 */
bool fs_grant_ops_ok(uint8_t have, uint8_t need);

/* vpath が g の範囲か。kind によって完全一致 / 前置一致。
   未知の kind は false (増やしたときに黙って前置一致へ落ちないため)。 */
bool fs_grant_in_scope(const fs_grant_scope_t *g, const char *vpath);

/* vpath が "/internal/data/<app>" とその下か (暗黙 grant の範囲)。 */
bool fs_grant_appdir_owns(const char *app, const char *vpath);

/* 判定の入口。
 *
 *   g        NULL なら「grant トークンを持っていない」。暗黙 grant だけが頼り
 *   app      呼び出し元アプリの名前。NULL / 不正なら暗黙 grant は当たらない
 *   vpath    対象の仮想パス
 *   need_ops この操作に要る FS_OP_* の or
 *
 * 予約サブツリーは grant より先に見る。design §4.6 の不変条件であって
 * 権限ではないので、どれだけ広い grant を持っていても変更は通らない。 */
fs_grant_verdict_t fs_grant_check(const fs_grant_scope_t *g, const char *app,
                                  const char *vpath, uint8_t need_ops);

/* ログと例外文言用。 */
const char *fs_grant_verdict_str(fs_grant_verdict_t v);

#ifdef __cplusplus
}
#endif

#endif /* FS_GRANT_H */
