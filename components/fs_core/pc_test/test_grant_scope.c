/*
 * test_grant_scope.c — grant 判定 (fs_grant.c) の境界。
 *
 * この判定は静かに間違える種類のコードなので、行ごとに「どの欠陥を捕まえる
 * ケースか」を書いてある。名前を書けないケースは、たぶん要らないケース。
 *
 * 狙って落としに来ているもの:
 *   - FILE を前置一致で書く   → 保存用の grant で "<name>.bak" まで書ける
 *   - 前置一致の直後の 1 文字を見ない → "/sd/doc" が "/sd/document" に届く
 *     (fs_reserved.c が実際に踏んだ罠。同じ規則をここでも使う)
 *   - APPDIR を「/internal/data 以下」で判定する → 他アプリの記録が読める
 *   - ops を「書き込みか否か」の 1 bit のまま扱う → CREATE と WRITE、
 *     DELETE と RENAME の区別が消える
 *   - 予約サブツリーを grant の後に見る → 広い grant が apps/ に届く
 *     (直近のコミット 99a1413 が塞いだ穴)
 *   - 逆向きの欠陥も見る: 予約を READ まで広げるとランチャーが
 *     /internal/apps を一覧できなくなる。安全側に倒れるので気づかれにくい。
 *
 * fs_grant.c も fs_reserved.c も ESP-IDF のヘッダを含まないので、この suite は
 * その 2 本だけをコンパイルして直接叩ける。
 */
#include <stdio.h>
#include <string.h>

#include "fs_grant.h"

static int t_checks;
static int t_fails;

/* ---- 表 ------------------------------------------------------------- */

typedef struct {
    const char        *why;      /* このケースが捕まえる欠陥 */
    int                has;      /* grant を持っているか。0 = 暗黙 grant だけ */
    uint8_t            kind;
    uint8_t            ops;
    const char        *root;
    const char        *app;      /* 呼び出し元アプリ名 (暗黙 grant 用) */
    const char        *path;
    uint8_t            need;
    fs_grant_verdict_t want;
} case_t;

#define G_ALL  FS_OP_ALL
#define RW     (FS_OP_READ | FS_OP_WRITE)
#define CW     (FS_OP_CREATE | FS_OP_WRITE)

static const case_t cases[] = {

/* ---- FILE は完全一致 ------------------------------------------------ */
/* 素直に通る形。これが赤いなら以下は全部意味が無い。 */
{ "FILE: 一致そのもの",
  1, FS_SCOPE_FILE, CW, "/internal/scripts/a.js", NULL,
  "/internal/scripts/a.js", FS_OP_WRITE, FS_GRANT_OK },
/* FILE を素の strncmp 前置一致で書いたときに通る形。本命。
   (この行だけは境界文字の判定でも止まるので、両方を壊して確認してある。
    下の "/a.js/x" は完全一致を捨てただけで通るので、そちらが本丸。) */
{ "FILE を素の strncmp にすると .bak まで書ける",
  1, FS_SCOPE_FILE, CW, "/internal/scripts/a.js", NULL,
  "/internal/scripts/a.js.bak", FS_OP_WRITE, FS_GRANT_DENY_SCOPE },
/* 同じく前置一致の穴。SAVE の grant がディレクトリ扱いされる形。 */
{ "FILE を前置一致にすると配下のパスまで書ける",
  1, FS_SCOPE_FILE, CW, "/internal/scripts/a.js", NULL,
  "/internal/scripts/a.js/x", FS_OP_WRITE, FS_GRANT_DENY_SCOPE },
/* 逆向き: 親ディレクトリへ登れてはいけない。 */
{ "FILE: 親ディレクトリ",
  1, FS_SCOPE_FILE, CW, "/internal/scripts/a.js", NULL,
  "/internal/scripts", FS_OP_WRITE, FS_GRANT_DENY_SCOPE },
/* 末尾 '/' は fsvol_resolve が落として同じファイルを開く。判定側だけが
   生の文字列で比べていると「判定した対象」と「開く対象」がずれる。 */
{ "FILE: path 側の末尾スラッシュは resolve と同じに正規化する",
  1, FS_SCOPE_FILE, CW, "/internal/scripts/a.js", NULL,
  "/internal/scripts/a.js/", FS_OP_WRITE, FS_GRANT_OK },
/* root 側にも同じ正規化が要る (同意画面から末尾スラッシュ付きで来る道がある)。 */
{ "FILE: root 側の末尾スラッシュ",
  1, FS_SCOPE_FILE, CW, "/internal/scripts/a.js/", NULL,
  "/internal/scripts/a.js", FS_OP_WRITE, FS_GRANT_OK },

/* ---- SUBTREE の境界文字 --------------------------------------------- */
{ "SUBTREE: root そのもの",
  1, FS_SCOPE_SUBTREE, G_ALL, "/sd/doc", NULL, "/sd/doc", FS_OP_READ, FS_GRANT_OK },
{ "SUBTREE: root + 末尾スラッシュ",
  1, FS_SCOPE_SUBTREE, G_ALL, "/sd/doc", NULL, "/sd/doc/", FS_OP_READ, FS_GRANT_OK },
{ "SUBTREE: 配下",
  1, FS_SCOPE_SUBTREE, G_ALL, "/sd/doc", NULL, "/sd/doc/x/y.txt", FS_OP_WRITE, FS_GRANT_OK },
/* 本命: strncmp だけで済ませると通る。 */
{ "SUBTREE: 境界文字を見ないと /sd/doc が /sd/document に届く",
  1, FS_SCOPE_SUBTREE, G_ALL, "/sd/doc", NULL, "/sd/document", FS_OP_WRITE, FS_GRANT_DENY_SCOPE },
{ "SUBTREE: 同上、その配下",
  1, FS_SCOPE_SUBTREE, G_ALL, "/sd/doc", NULL, "/sd/document/x", FS_OP_WRITE, FS_GRANT_DENY_SCOPE },
{ "SUBTREE: 短いパス (pn < rn を見ないと strncmp が終端で一致する)",
  1, FS_SCOPE_SUBTREE, G_ALL, "/sd/doc", NULL, "/sd/do", FS_OP_READ, FS_GRANT_DENY_SCOPE },
{ "SUBTREE: 親へ登る",
  1, FS_SCOPE_SUBTREE, G_ALL, "/sd/doc", NULL, "/sd", FS_OP_READ, FS_GRANT_DENY_SCOPE },
/* ボリューム id を含めて比べていないと、内蔵と SD の同名パスが混ざる。 */
{ "SUBTREE: 別ボリュームの同名サブパス",
  1, FS_SCOPE_SUBTREE, G_ALL, "/sd/doc", NULL, "/internal/doc/x", FS_OP_READ, FS_GRANT_DENY_SCOPE },
{ "SUBTREE: ボリューム丸ごとの grant",
  1, FS_SCOPE_SUBTREE, G_ALL, "/sd", NULL, "/sd/anything/deep", FS_OP_WRITE, FS_GRANT_OK },

/* ---- APPDIR (明示的に mint されたもの) ------------------------------- */
{ "APPDIR: 自分の配下",
  1, FS_SCOPE_APPDIR, G_ALL, "/internal/data/reading", NULL,
  "/internal/data/reading/log.json", FS_OP_WRITE, FS_GRANT_OK },
/* 本命: 他アプリのディレクトリへの越境。 */
{ "APPDIR: 他アプリのディレクトリへ越境",
  1, FS_SCOPE_APPDIR, G_ALL, "/internal/data/reading", NULL,
  "/internal/data/circuit/log.json", FS_OP_READ, FS_GRANT_DENY_SCOPE },
{ "APPDIR: 境界文字 (reading が reading-notes に届く)",
  1, FS_SCOPE_APPDIR, G_ALL, "/internal/data/reading", NULL,
  "/internal/data/reading-notes/x", FS_OP_READ, FS_GRANT_DENY_SCOPE },
{ "APPDIR: 親 (/internal/data) へ登る",
  1, FS_SCOPE_APPDIR, G_ALL, "/internal/data/reading", NULL,
  "/internal/data", FS_OP_READ, FS_GRANT_DENY_SCOPE },
/* kind を増やしたとき、default が前置一致に落ちると黙って広がる。 */
{ "未知の kind は通さない",
  1, 3, G_ALL, "/sd", NULL, "/sd/x", FS_OP_READ, FS_GRANT_DENY_SCOPE },

/* ---- ops ビット ------------------------------------------------------ */
{ "ops: 読み取り専用 grant で書けない",
  1, FS_SCOPE_SUBTREE, FS_OP_READ, "/sd", NULL, "/sd/x", FS_OP_WRITE, FS_GRANT_DENY_OPS },
{ "ops: 読み取り専用 grant で読める",
  1, FS_SCOPE_SUBTREE, FS_OP_READ, "/sd", NULL, "/sd/x", FS_OP_READ, FS_GRANT_OK },
/* CREATE と WRITE を 1 bit に潰すと通る形。新規作成と上書きの差。 */
{ "ops: CREATE 不足 (新規 write は CREATE|WRITE が要る)",
  1, FS_SCOPE_SUBTREE, RW, "/sd", NULL, "/sd/new.txt", CW, FS_GRANT_DENY_OPS },
{ "ops: 過剰な grant は通る (包含であって一致ではない)",
  1, FS_SCOPE_SUBTREE, CW, "/sd", NULL, "/sd/x", FS_OP_WRITE, FS_GRANT_OK },
{ "ops: DELETE を持っていれば消せる",
  1, FS_SCOPE_SUBTREE, G_ALL, "/sd", NULL, "/sd/x", FS_OP_DELETE, FS_GRANT_OK },
{ "ops: DELETE 不足 (書ける = 消せる にしない)",
  1, FS_SCOPE_SUBTREE, (FS_OP_READ | FS_OP_CREATE | FS_OP_WRITE), "/sd", NULL,
  "/sd/x", FS_OP_DELETE, FS_GRANT_DENY_OPS },
{ "ops: RENAME 不足 (消せる = 名前を変えられる にしない)",
  1, FS_SCOPE_SUBTREE, (FS_OP_READ | FS_OP_CREATE | FS_OP_WRITE | FS_OP_DELETE),
  "/sd", NULL, "/sd/x", FS_OP_RENAME, FS_GRANT_DENY_OPS },
{ "ops: 何も持たない grant",
  1, FS_SCOPE_SUBTREE, 0, "/sd", NULL, "/sd/x", FS_OP_READ, FS_GRANT_DENY_OPS },
{ "ops: 複数ビットの要求は全部揃って初めて通る",
  1, FS_SCOPE_SUBTREE, G_ALL, "/sd", NULL, "/sd/x", (FS_OP_READ | FS_OP_RENAME), FS_GRANT_OK },
/* need=0 を「制約なし」と読むと、op を渡し忘れた呼び出しが素通りする。 */
{ "ops: need が 0 の呼び出しは拒否する",
  1, FS_SCOPE_SUBTREE, G_ALL, "/sd", NULL, "/sd/x", 0, FS_GRANT_DENY_BADREQ },
{ "ops: 未定義ビットを含む要求は拒否する",
  1, FS_SCOPE_SUBTREE, G_ALL, "/sd", NULL, "/sd/x", 0x20, FS_GRANT_DENY_BADREQ },
{ "ops: 既知ビット + 未定義ビット",
  1, FS_SCOPE_SUBTREE, G_ALL, "/sd", NULL, "/sd/x", (uint8_t)(FS_OP_READ | 0x80),
  FS_GRANT_DENY_BADREQ },

/* ---- 予約サブツリーが勝つ ------------------------------------------- */
/* 本命 (99a1413)。"/internal" を丸ごと許した同意でも apps/ には届かない。 */
{ "予約: /internal 丸ごとの grant でも apps/ に書けない",
  1, FS_SCOPE_SUBTREE, G_ALL, "/internal", NULL,
  "/internal/apps/evil.mjsa", CW, FS_GRANT_DENY_RESERVED },
{ "予約: 削除も通さない",
  1, FS_SCOPE_SUBTREE, G_ALL, "/internal", NULL,
  "/internal/apps/reading.mjsa", FS_OP_DELETE, FS_GRANT_DENY_RESERVED },
{ "予約: rename も通さない",
  1, FS_SCOPE_SUBTREE, G_ALL, "/internal", NULL,
  "/internal/apps", FS_OP_RENAME, FS_GRANT_DENY_RESERVED },
{ "予約: 末尾スラッシュで素通りしない",
  1, FS_SCOPE_SUBTREE, G_ALL, "/internal", NULL,
  "/internal/apps/", FS_OP_WRITE, FS_GRANT_DENY_RESERVED },
/* 逆向きの欠陥: READ まで予約するとランチャーがアプリを一覧できなくなる。
   fs_core.h が「予約するのは変更だけ」と明言している。 */
{ "予約: READ は通す (予約するのは変更だけ)",
  1, FS_SCOPE_SUBTREE, G_ALL, "/internal", NULL,
  "/internal/apps/reading.mjsa", FS_OP_READ, FS_GRANT_OK },
/* 逆向きの欠陥その 2: 予約を前置一致で書くと、ユーザの作った
   /internal/appsfoo が二度と消せなくなる。 */
{ "予約: /internal/appsfoo はユーザのフォルダ",
  1, FS_SCOPE_SUBTREE, G_ALL, "/internal", NULL,
  "/internal/appsfoo/x", FS_OP_DELETE, FS_GRANT_OK },
/* 予約は grant の有無より先。grant が無い経路でも同じ判定になること。 */
{ "予約: grant を持たない呼び出しでも予約が先に効く",
  0, 0, 0, NULL, "reading", "/internal/apps/x.mjsa", FS_OP_WRITE, FS_GRANT_DENY_RESERVED },
/* SD の apps は予約しない (使うたびに署名を確かめるので)。 */
{ "予約: /sd/apps は予約しない",
  1, FS_SCOPE_SUBTREE, G_ALL, "/sd", NULL, "/sd/apps/x.mjsa", FS_OP_DELETE, FS_GRANT_OK },

/* ---- 暗黙 grant (gv == 0) -------------------------------------------- */
{ "暗黙: 自分の私有ディレクトリへ書ける",
  0, 0, 0, NULL, "reading", "/internal/data/reading/log.json", FS_OP_WRITE, FS_GRANT_OK },
{ "暗黙: 私有なので削除も rename もできる",
  0, 0, 0, NULL, "reading", "/internal/data/reading/log.json",
  (FS_OP_DELETE | FS_OP_RENAME), FS_GRANT_OK },
{ "暗黙: ディレクトリ自身 (mkdir できないと私有ディレクトリを作れない)",
  0, 0, 0, NULL, "reading", "/internal/data/reading", FS_OP_CREATE, FS_GRANT_OK },
{ "暗黙: ディレクトリ自身 + 末尾スラッシュ",
  0, 0, 0, NULL, "reading", "/internal/data/reading/", FS_OP_READ, FS_GRANT_OK },
/* 本命: 他アプリの記録。 */
{ "暗黙: 他アプリのディレクトリには届かない",
  0, 0, 0, NULL, "reading", "/internal/data/circuit/log.json", FS_OP_READ,
  FS_GRANT_DENY_NO_GRANT },
{ "暗黙: 境界文字 (reading が reading-notes に届く)",
  0, 0, 0, NULL, "reading", "/internal/data/reading-notes/x", FS_OP_READ,
  FS_GRANT_DENY_NO_GRANT },
/* "/internal/data 以下なら通す" と書いたときに通ってしまう形。 */
{ "暗黙: /internal/data そのもの (アプリ名が無い)",
  0, 0, 0, NULL, "reading", "/internal/data", FS_OP_READ, FS_GRANT_DENY_NO_GRANT },
{ "暗黙: /internal/data/ そのもの",
  0, 0, 0, NULL, "reading", "/internal/data/", FS_OP_READ, FS_GRANT_DENY_NO_GRANT },
{ "暗黙: /internal/datafoo (前置一致の罠)",
  0, 0, 0, NULL, "reading", "/internal/datafoo/x", FS_OP_READ, FS_GRANT_DENY_NO_GRANT },
{ "暗黙: 別ボリュームには及ばない",
  0, 0, 0, NULL, "reading", "/sd/data/reading/x", FS_OP_READ, FS_GRANT_DENY_NO_GRANT },
{ "暗黙: 内蔵の別の場所には及ばない",
  0, 0, 0, NULL, "reading", "/internal/scripts/a.js", FS_OP_READ, FS_GRANT_DENY_NO_GRANT },
{ "暗黙: アプリ名が無い (App record が付く前の呼び出し)",
  0, 0, 0, NULL, NULL, "/internal/data/reading/x", FS_OP_READ, FS_GRANT_DENY_NO_GRANT },
/* 空名を許すと /internal/data 以下が全部 1 アプリの私有になる。 */
{ "暗黙: 空のアプリ名は /internal/data 丸ごとにならない",
  0, 0, 0, NULL, "", "/internal/data/x", FS_OP_READ, FS_GRANT_DENY_NO_GRANT },
/* 「サニタイズ済み」の前提が破れた場合。名前側でも止める。 */
{ "暗黙: アプリ名にスラッシュ (隔離が名前の付け方次第で崩れる)",
  0, 0, 0, NULL, "a/b", "/internal/data/a/b/x", FS_OP_READ, FS_GRANT_DENY_NO_GRANT },
{ "暗黙: アプリ名が ..",
  0, 0, 0, NULL, "..", "/internal/data/x", FS_OP_READ, FS_GRANT_DENY_NO_GRANT },
{ "暗黙: アプリ名が .",
  0, 0, 0, NULL, ".", "/internal/data/x", FS_OP_READ, FS_GRANT_DENY_NO_GRANT },
/* 名前が壊れていても、脱出はパス側で先に止まる (二重に止める)。 */
{ "暗黙: .. を含むパスは名前を見る前に落ちる",
  0, 0, 0, NULL, "..", "/internal/data/../apps/x", FS_OP_WRITE, FS_GRANT_DENY_BADPATH },
{ "暗黙: 大文字小文字は区別する",
  0, 0, 0, NULL, "Reading", "/internal/data/reading/x", FS_OP_READ,
  FS_GRANT_DENY_NO_GRANT },

/* ---- パスの形 -------------------------------------------------------- */
/* grant を見てから fs_ops を呼ぶので、判定の時点では誰もパスを検査して
   いない。判定が resolve より緩いと、判定を通ったあとで別物が開かれる。 */
{ "形: NULL",
  1, FS_SCOPE_SUBTREE, G_ALL, "/sd", NULL, NULL, FS_OP_READ, FS_GRANT_DENY_BADPATH },
{ "形: 空文字列",
  1, FS_SCOPE_SUBTREE, G_ALL, "/sd", NULL, "", FS_OP_READ, FS_GRANT_DENY_BADPATH },
{ "形: 相対パス",
  1, FS_SCOPE_SUBTREE, G_ALL, "/sd", NULL, "sd/x", FS_OP_READ, FS_GRANT_DENY_BADPATH },
{ "形: スラッシュだけ",
  1, FS_SCOPE_SUBTREE, G_ALL, "/sd", NULL, "/", FS_OP_READ, FS_GRANT_DENY_BADPATH },
{ "形: 先頭の二重スラッシュ",
  1, FS_SCOPE_SUBTREE, G_ALL, "/sd", NULL, "//sd/x", FS_OP_READ, FS_GRANT_DENY_BADPATH },
{ "形: 途中の二重スラッシュ",
  1, FS_SCOPE_SUBTREE, G_ALL, "/sd", NULL, "/sd//x", FS_OP_READ, FS_GRANT_DENY_BADPATH },
/* 本命: これが通ると SUBTREE /sd の grant で予約領域まで届く。 */
{ "形: .. で範囲を抜ける",
  1, FS_SCOPE_SUBTREE, G_ALL, "/sd", NULL, "/sd/../internal/apps/x", CW,
  FS_GRANT_DENY_BADPATH },
{ "形: . セグメント",
  1, FS_SCOPE_SUBTREE, G_ALL, "/sd", NULL, "/sd/./x", FS_OP_READ, FS_GRANT_DENY_BADPATH },
{ "形: 全ドットのセグメント",
  1, FS_SCOPE_SUBTREE, G_ALL, "/sd", NULL, "/sd/...", FS_OP_READ, FS_GRANT_DENY_BADPATH },
{ "形: 制御文字 (タブ)",
  1, FS_SCOPE_SUBTREE, G_ALL, "/sd", NULL, "/sd/x\ty", FS_OP_READ, FS_GRANT_DENY_BADPATH },
{ "形: DEL",
  1, FS_SCOPE_SUBTREE, G_ALL, "/sd", NULL, "/sd/x\x7fy", FS_OP_READ, FS_GRANT_DENY_BADPATH },
/* 過剰に弾いていないことの確認。"x." は全ドットではない。 */
{ "形: 末尾がドットの名前は正当",
  1, FS_SCOPE_SUBTREE, G_ALL, "/sd", NULL, "/sd/x.", FS_OP_READ, FS_GRANT_OK },
{ "形: 名前に空白や UTF-8 が入っても正当",
  1, FS_SCOPE_SUBTREE, G_ALL, "/sd", NULL, "/sd/読書 記録.txt", FS_OP_READ, FS_GRANT_OK },

/* ---- root の形 ------------------------------------------------------- */
/* root がスラッシュ 1 文字のまま入ってきたら全ボリュームが私有になる。 */
{ "root: スラッシュだけの root は万能にならない",
  1, FS_SCOPE_SUBTREE, G_ALL, "/", NULL, "/sd/x", FS_OP_READ, FS_GRANT_DENY_SCOPE },
{ "root: 空文字列",
  1, FS_SCOPE_SUBTREE, G_ALL, "", NULL, "/sd/x", FS_OP_READ, FS_GRANT_DENY_SCOPE },
{ "root: 相対",
  1, FS_SCOPE_SUBTREE, G_ALL, "sd", NULL, "/sd/x", FS_OP_READ, FS_GRANT_DENY_SCOPE },
{ "root: 形が壊れていれば判定しない",
  1, FS_SCOPE_SUBTREE, G_ALL, "/sd/../internal", NULL, "/internal/x", FS_OP_READ,
  FS_GRANT_DENY_SCOPE },
{ "root: 大文字小文字は区別する",
  1, FS_SCOPE_SUBTREE, G_ALL, "/sd/Doc", NULL, "/sd/doc/x", FS_OP_READ,
  FS_GRANT_DENY_SCOPE },
};

/* ---- 実行 ------------------------------------------------------------ */

static fs_grant_scope_t mk(uint8_t kind, uint8_t ops, const char *root)
{
    fs_grant_scope_t g;
    memset(&g, 0, sizeof g);
    g.kind = kind;
    g.ops  = ops;
    if (root) {
        size_t n = strlen(root);
        if (n >= sizeof g.root)
            n = sizeof g.root - 1;
        memcpy(g.root, root, n);
    }
    return g;
}

static void report(const char *why, const char *path, fs_grant_verdict_t want,
                   fs_grant_verdict_t got)
{
    t_checks++;
    if (want == got) {
        printf("ok    %-26s %-22s | %s\n", path ? path : "(null)",
               fs_grant_verdict_str(got), why);
    } else {
        t_fails++;
        printf("FAIL  %-26s want %-22s got %-22s | %s\n", path ? path : "(null)",
               fs_grant_verdict_str(want), fs_grant_verdict_str(got), why);
    }
}

static void run_table(void)
{
    printf("=== fs_grant_check ===\n");
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        const case_t *c = &cases[i];
        fs_grant_scope_t g = mk(c->kind, c->ops, c->root);
        fs_grant_verdict_t got =
            fs_grant_check(c->has ? &g : NULL, c->app, c->path, c->need);
        report(c->why, c->path, c->want, got);
    }
}

/* 表に入れられないもの: 長さの端、終端していない root、述語の直接確認。 */

static void expect_bool(const char *why, bool want, bool got)
{
    t_checks++;
    if (want == got) {
        printf("ok    %-49s | %s\n", got ? "true" : "false", why);
    } else {
        t_fails++;
        printf("FAIL  want %-5s got %-5s %-31s | %s\n", want ? "true" : "false",
               got ? "true" : "false", "", why);
    }
}

static void run_lengths(void)
{
    static char buf[FS_GRANT_PATH_MAX + 8];

    printf("\n=== 長さの端 ===\n");

    /* ちょうど FS_GRANT_PATH_MAX は通る。1 バイト超えると通らない。
       resolve 側と同じ上限にしていないと、判定を通ったパスが開けない
       (安全側だが、原因の分からない失敗になる)。
       セグメント長の上限に引っかからないよう 64 バイトごとに区切る。 */
    memset(buf, 'a', sizeof buf);
    for (size_t i = 0; i < sizeof buf; i += 64)
        buf[i] = '/';
    buf[FS_GRANT_PATH_MAX] = '\0';                 /* 長さちょうど PATH_MAX */
    expect_bool("形: ちょうど FS_GRANT_PATH_MAX 文字", true, fs_grant_path_ok(buf));
    buf[FS_GRANT_PATH_MAX] = 'a';
    buf[FS_GRANT_PATH_MAX + 1] = '\0';
    expect_bool("形: FS_GRANT_PATH_MAX + 1 文字", false, fs_grant_path_ok(buf));

    /* 1 セグメントが FS_GRANT_NAME_MAX を超える。"/aa/" + 'a' × (NAME_MAX+1)。 */
    memset(buf, 'a', sizeof buf);
    buf[0] = '/';
    buf[3] = '/';
    buf[4 + FS_GRANT_NAME_MAX + 1] = '\0';
    expect_bool("形: セグメントが FS_GRANT_NAME_MAX を超える", false,
                fs_grant_path_ok(buf));
    buf[4 + FS_GRANT_NAME_MAX] = '\0';             /* ちょうど NAME_MAX は通る */
    expect_bool("形: セグメントがちょうど FS_GRANT_NAME_MAX", true,
                fs_grant_path_ok(buf));

    /* root が器の中で終端していない (runtime 側の MQJS_FS_SCOPE_MAX と
       ずれた場合)。strlen が器を飛び出す前に拒否側へ倒れること。 */
    fs_grant_scope_t g;
    memset(&g, 'x', sizeof g);                     /* root 全体が非 NUL */
    g.kind = FS_SCOPE_SUBTREE;
    g.ops  = FS_OP_ALL;
    g.root[0] = '/';
    expect_bool("root が器の中で終端していなければ拒否", false,
                fs_grant_in_scope(&g, "/xxx"));
    report("同上: check の判定", "/xxx", FS_GRANT_DENY_SCOPE,
           fs_grant_check(&g, NULL, "/xxx", FS_OP_READ));
}

static void run_predicates(void)
{
    printf("\n=== 述語 ===\n");

    /* 呼び出し側が壊れていても落ちない。 */
    expect_bool("path_ok(NULL)", false, fs_grant_path_ok(NULL));
    expect_bool("name_ok(NULL)", false, fs_grant_name_ok(NULL));
    expect_bool("in_scope(NULL grant)", false, fs_grant_in_scope(NULL, "/sd/x"));
    expect_bool("appdir_owns(NULL, NULL)", false, fs_grant_appdir_owns(NULL, NULL));

    /* ops は包含。一致でも「どちらかのビット」でもない。 */
    expect_bool("ops: ALL は WRITE を含む", true,
                fs_grant_ops_ok(FS_OP_ALL, FS_OP_WRITE));
    expect_bool("ops: READ は WRITE を含まない", false,
                fs_grant_ops_ok(FS_OP_READ, FS_OP_WRITE));
    expect_bool("ops: 部分一致 (READ|WRITE 要求に READ だけ) は通さない", false,
                fs_grant_ops_ok(FS_OP_READ, FS_OP_READ | FS_OP_WRITE));
    expect_bool("ops: need 0 は通さない", false, fs_grant_ops_ok(FS_OP_ALL, 0));
    expect_bool("ops: have 0 need 0", false, fs_grant_ops_ok(0, 0));

    /* 暗黙 grant の範囲そのもの。 */
    expect_bool("appdir: 自分の下", true,
                fs_grant_appdir_owns("reading", "/internal/data/reading/a/b"));
    expect_bool("appdir: 名前の前置一致 (read が reading に届く)", false,
                fs_grant_appdir_owns("read", "/internal/data/reading/a"));
    expect_bool("appdir: 名前が長い側", false,
                fs_grant_appdir_owns("reading", "/internal/data/read"));
}

int main(void)
{
    run_table();
    run_lengths();
    run_predicates();

    printf("\n%s  %d checks, %d failures\n",
           t_fails ? "SUITE-FAIL" : "SUITE-OK", t_checks, t_fails);
    return t_fails ? 1 : 0;
}
