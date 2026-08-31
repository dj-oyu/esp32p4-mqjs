/*
 * grant の判定。表は持たない (fs_grant.h 冒頭)。
 *
 * ESP-IDF のヘッダを 1 本も含めないのは fs_reserved.c と同じ理由で、
 * 境界の判定をホストの gcc でそのまま叩けるようにするため。
 * pc_test/test_grant_scope.c がこの .c を直接コンパイルする。
 */
#include "fs_grant.h"

#include <string.h>

/* fs_core.h からではなく、ここで宣言する。fs_core.h は esp_err.h を引くので
   ホストでは開けない (test_reserved.c が同じことをしている)。実体は
   fs_reserved.c で、ESP 側でもホスト側でも同じ .c がリンクされる。 */
bool fs_path_reserved(const char *vpath);

/* strnlen は C99 に無い (POSIX 2008)。ホストの -std=c99 で暗黙宣言に
   なるのを避けるため自前で持つ。 */
static size_t bounded_len(const char *s, size_t cap)
{
    size_t n = 0;
    while (n < cap && s[n])
        n++;
    return n;                      /* cap を返したら「cap 内で終端していない」 */
}

/* 末尾の '/' を落とした実効長。fsvol_resolve が実パスを組むときに同じ
   正規化をするので (fs_vol.c: rest の末尾 '/' を落とす)、判定側だけが
   生の文字列で比べていると「判定した対象」と「開く対象」が別物になる。
   root 側も path 側も同じ関数を通す。 */
static size_t eff_len(const char *s, size_t n)
{
    while (n > 1 && s[n - 1] == '/')
        n--;
    return n;
}

bool fs_grant_path_ok(const char *vpath)
{
    if (!vpath || vpath[0] != '/')
        return false;                              /* 相対パスもここで落ちる */

    size_t n = bounded_len(vpath, FS_GRANT_PATH_MAX + 1);
    if (n > FS_GRANT_PATH_MAX)
        return false;                              /* 長すぎ、または終端していない */

    /* ボリューム id のセグメントが空 ("/" や "//x") なら resolve が通さない。 */
    if (vpath[1] == '\0' || vpath[1] == '/')
        return false;

    /* 全体を一度なめる。fs_vol.c の path_shape_ok と同じ規則。 */
    size_t seg = 0;
    bool   all_dots = true;
    for (size_t i = 1; i <= n; i++) {
        char c = vpath[i];
        if (c == '/' || c == '\0') {
            if (seg == 0 && i != n)                /* "//" — 末尾の '/' だけ許す */
                return false;
            if (seg > 0 && all_dots)               /* "." ".." "..." */
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
        if (seg > FS_GRANT_NAME_MAX)
            return false;
    }
    return true;
}

/* 暗黙 grant の <app> は「サニタイズ済みの App 名」という前提で仕様が
   書かれている (spec §A.5)。前提が破れたときに何が起きるかを書いておく:
 *
 *   - '/' を含む名前 ("a/b") … /internal/data/a/b 以下が私有になるだけで
 *     越境はしない。が、別アプリ "a" の私有ディレクトリの内側を指すので、
 *     アプリ間の隔離が名前の付け方次第で崩れる。だから拒否する。
 *   - ".." … これ単体では何も起きない。パス側に ".." が現れないと脱出に
 *     ならず、それは fs_grant_path_ok が拒否する。二重に止める。
 *   - "" (空) … "/internal/data/" 以下すべてが私有になる。全アプリの記録が
 *     1 つのアプリから読める。拒否する。
 *
 * つまり「サニタイズ済み」を信じない。信じなくても安いので。 */
bool fs_grant_name_ok(const char *app)
{
    if (!app)
        return false;
    size_t n = bounded_len(app, FS_GRANT_NAME_MAX + 1);
    if (n == 0 || n > FS_GRANT_NAME_MAX)
        return false;

    bool all_dots = true;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)app[i];
        if (c == '/' || c < 0x20 || c == 0x7f)
            return false;
        if (c != '.')
            all_dots = false;
    }
    return !all_dots;                              /* "." ".." "..." を拒否 */
}

bool fs_grant_ops_ok(uint8_t have, uint8_t need)
{
    if (need == 0)
        return false;                              /* 何が要るか言わない呼び出し */
    return (uint8_t)(have & need) == need;
}

bool fs_grant_in_scope(const fs_grant_scope_t *g, const char *vpath)
{
    if (!g)
        return false;

    /* root が器の中で終端していなければ判定しない。runtime 側の
       MQJS_FS_SCOPE_MAX とずれたとき、ここで拒否側へ倒れる。 */
    if (bounded_len(g->root, FS_GRANT_ROOT_MAX) >= FS_GRANT_ROOT_MAX)
        return false;
    if (!fs_grant_path_ok(g->root) || !fs_grant_path_ok(vpath))
        return false;

    size_t rn = eff_len(g->root, strlen(g->root));
    size_t pn = eff_len(vpath, strlen(vpath));

    switch (g->kind) {
    case FS_SCOPE_FILE:
        /* 完全一致。前置一致に緩めると、"/internal/scripts/a.js" を保存する
           ために出した grant で "/internal/scripts/a.js.bak" まで書ける。 */
        return rn == pn && strncmp(vpath, g->root, rn) == 0;

    case FS_SCOPE_SUBTREE:
    case FS_SCOPE_APPDIR:
        if (pn < rn || strncmp(vpath, g->root, rn) != 0)
            return false;
        /* 前置一致の直後の 1 文字。ここを見ないと root="/sd/doc" が
           "/sd/document" に届く (fs_reserved.c が同じ罠を踏んだ場所)。 */
        return pn == rn || vpath[rn] == '/';

    default:
        return false;                              /* 未知の kind は通さない */
    }
}

bool fs_grant_appdir_owns(const char *app, const char *vpath)
{
    static const char base[] = "/internal/data/";
    const size_t bn = sizeof base - 1;

    if (!fs_grant_name_ok(app) || !fs_grant_path_ok(vpath))
        return false;

    /* base が '/' で終わっているので、"/internal/datafoo/x" はここで落ちる。 */
    if (strncmp(vpath, base, bn) != 0)
        return false;

    size_t an = strlen(app);
    if (strncmp(vpath + bn, app, an) != 0)
        return false;

    /* 直後の 1 文字。これが無いと "reading" の暗黙 grant が
       "/internal/data/reading-notes" に届く。 */
    char c = vpath[bn + an];
    return c == '\0' || c == '/';
}

fs_grant_verdict_t fs_grant_check(const fs_grant_scope_t *g, const char *app,
                                  const char *vpath, uint8_t need_ops)
{
    if (!fs_grant_path_ok(vpath))
        return FS_GRANT_DENY_BADPATH;
    if (need_ops == 0 || (need_ops & (uint8_t)~(unsigned)FS_OP_ALL))
        return FS_GRANT_DENY_BADREQ;

    /* 権限より先に不変条件 (design §4.6)。grant の有無も種別も見ない。
       READ は対象外 — 予約するのは変更だけ (fs_core.h)。 */
    if ((need_ops & FS_OP_MUTATE) && fs_path_reserved(vpath))
        return FS_GRANT_DENY_RESERVED;

    uint8_t have;
    if (g) {
        if (!fs_grant_in_scope(g, vpath))
            return FS_GRANT_DENY_SCOPE;
        have = g->ops;
    } else {
        /* 暗黙 grant。表には入れない (spec §A.5) ので、ここが唯一の判定。
           私有ディレクトリなので全操作を持つ ── 記録を書いて消せなければ
           同意画面を避けた意味が無い。 */
        if (!fs_grant_appdir_owns(app, vpath))
            return FS_GRANT_DENY_NO_GRANT;
        have = FS_OP_ALL;
    }

    if (!fs_grant_ops_ok(have, need_ops))
        return FS_GRANT_DENY_OPS;
    return FS_GRANT_OK;
}

const char *fs_grant_verdict_str(fs_grant_verdict_t v)
{
    switch (v) {
    case FS_GRANT_OK:             return "ok";
    case FS_GRANT_DENY_RESERVED:  return "reserved subtree";
    case FS_GRANT_DENY_SCOPE:     return "outside the granted scope";
    case FS_GRANT_DENY_OPS:       return "operation not granted";
    case FS_GRANT_DENY_NO_GRANT:  return "no grant";
    case FS_GRANT_DENY_BADPATH:   return "bad path";
    case FS_GRANT_DENY_BADREQ:    return "bad request";
    }
    return "?";
}
