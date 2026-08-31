/*
 * edit_test.h — ホストテスト共通の足場。
 *
 * 方針: 1 件失敗しても止めない (fs_core/pc_test/test_reserved.c と同じ)。
 * 最初の 1 件で abort すると「その先が全部通るのか、そこで死んだのか」が
 * 分からず、仕様との突き合わせに使えないため。ただしブロック確保や
 * edit_init に失敗したときだけは、それ以降の check が全部無意味になるので
 * 即返す。
 *
 * このヘッダ自体は何も検証しない。検証は test_*.c にある。
 *
 * 見ていないもの (この足場の限界):
 *  - 実機のアラインメント要求。malloc は 16 B 境界を返すので、
 *    「block は 8B 整列」(§A.1) を守らない呼び出し側は再現できない。
 *  - スレッド安全性。edit_core は単一タスク前提 (§A.2) なので、
 *    ホストテストは全部シングルスレッド。
 *  - 本文に NUL バイトを含むケース。ET_IS/et_text は C 文字列で比較する。
 *    NUL 入りは fuzz_edit.c の担当。
 */
#ifndef EDIT_TEST_H
#define EDIT_TEST_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "edit_core.h"

/* ------------------------------------------------------------------ 集計 */

static int et_checks;
static int et_fails;

#define ET_CHECK(cond, ...)                                                   \
    do {                                                                      \
        et_checks++;                                                          \
        if (!(cond)) {                                                        \
            et_fails++;                                                       \
            printf("FAIL  %s:%d  [%s]  ", __FILE__, __LINE__, #cond);         \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                     \
        }                                                                     \
    } while (0)

/* 条件を持たない、無条件の失敗 (到達してはいけない場所) */
#define ET_FAILED(...)                                                        \
    do {                                                                      \
        et_checks++;                                                          \
        et_fails++;                                                           \
        printf("FAIL  %s:%d  ", __FILE__, __LINE__);                          \
        printf(__VA_ARGS__);                                                  \
        printf("\n");                                                         \
    } while (0)

#define ET_ERR(call, want)                                                    \
    do {                                                                      \
        edit_err_t et__got = (call);                                          \
        et_checks++;                                                          \
        if (et__got != (want)) {                                              \
            et_fails++;                                                       \
            printf("FAIL  %s:%d  %s -> %s, want %s\n", __FILE__, __LINE__,    \
                   #call, et_err_name(et__got), et_err_name(want));           \
        }                                                                     \
    } while (0)

#define ET_SUMMARY(title)                                                     \
    do {                                                                      \
        printf("%s  %s  %d checks, %d failures\n",                            \
               et_fails ? "SUITE-FAIL" : "SUITE-OK", (title),                 \
               et_checks, et_fails);                                          \
    } while (0)

static inline const char *et_err_name(edit_err_t e)
{
    switch (e) {
    case EDIT_OK:       return "EDIT_OK";
    case EDIT_E_ARG:    return "EDIT_E_ARG";
    case EDIT_E_FULL:   return "EDIT_E_FULL";
    case EDIT_E_LINES:  return "EDIT_E_LINES";
    case EDIT_E_UTF8:   return "EDIT_E_UTF8";
    case EDIT_E_STATE:  return "EDIT_E_STATE";
    default:            return "(unknown)";
    }
}

static inline const char *et_cls_name(unsigned c)
{
    switch (c) {
    case EDIT_CLS_PLAIN:   return "PLAIN";
    case EDIT_CLS_KEYWORD: return "KEYWORD";
    case EDIT_CLS_IDENT:   return "IDENT";
    case EDIT_CLS_NUMBER:  return "NUMBER";
    case EDIT_CLS_STRING:  return "STRING";
    case EDIT_CLS_COMMENT: return "COMMENT";
    case EDIT_CLS_PUNCT:   return "PUNCT";
    case EDIT_CLS_PREEDIT: return "PREEDIT";
    default:               return "(unknown)";
    }
}

/* --------------------------------------------------------- インスタンス */

typedef struct {
    void   *block;
    size_t  block_len;
    edit_t *e;
} et_ed_t;

static inline edit_config_t et_cfg(void)
{
    edit_config_t c;
    c.max_bytes = 8192;
    c.max_lines = 64;
    c.undo_bytes = 4096;
    c.max_cols = 40;
    c.max_rows = 8;
    return c;
}

/*
 * ブロックは edit_mem_size() ちょうどの大きさで malloc する。余分に取ると
 * ASAN が末尾のはみ出しを見逃す。
 *
 * 開いた直後に set_view(max_cols, max_rows) を 1 回呼び、dirty をクリアする:
 * §A.1 は set_view 前の view の既定値を決めていないので、そこに依存する
 * テストを書かないための予防。view そのものを見る test_view.c は自分で
 * set_view を呼び直す。
 */
static inline bool et_open(et_ed_t *ed, const edit_config_t *cfg)
{
    edit_err_t err;

    memset(ed, 0, sizeof(*ed));
    ed->block_len = edit_mem_size(cfg);
    et_checks++;
    if (ed->block_len == 0) {
        et_fails++;
        printf("FAIL  edit_mem_size() returned 0\n");
        return false;
    }
    ed->block = malloc(ed->block_len);
    if (!ed->block) {
        et_checks++;
        et_fails++;
        printf("FAIL  malloc(%zu) failed\n", ed->block_len);
        return false;
    }
    memset(ed->block, 0xA5, ed->block_len);
    err = edit_init(ed->block, ed->block_len, cfg, &ed->e);
    et_checks++;
    if (err != EDIT_OK || !ed->e) {
        et_fails++;
        printf("FAIL  edit_init -> %s (e=%p)\n", et_err_name(err), (void *)ed->e);
        free(ed->block);
        ed->block = NULL;
        return false;
    }
    edit_set_view(ed->e, cfg->max_cols, cfg->max_rows);
    edit_dirty_clear(ed->e);
    return true;
}

static inline bool et_open_default(et_ed_t *ed)
{
    edit_config_t c = et_cfg();
    return et_open(ed, &c);
}

static inline void et_close(et_ed_t *ed)
{
    free(ed->block);
    ed->block = NULL;
    ed->e = NULL;
}

/* ------------------------------------------------------------- 本文の読み */

/* 本文全体を dst へ取り出して NUL 終端する。返り値はバイト数。
   dst には len+1 バイト以上が要る (§A.1 は copy_text の NUL 終端を
   約束していないので、こちらで足す)。 */
static inline size_t et_text(const edit_t *e, char *dst, size_t cap)
{
    size_t len = edit_text_len(e);
    size_t n;
    if (len + 1 > cap) {
        et_checks++;
        et_fails++;
        printf("FAIL  et_text: buffer too small (%zu needed, %zu given)\n",
               len + 1, cap);
        if (cap) dst[0] = 0;
        return 0;
    }
    n = edit_copy_text(e, 0, dst, len);
    dst[n] = 0;
    return n;
}

/* 本文が want と一致するか。want は NUL 終端の C 文字列。 */
static inline void et_is_at(const edit_t *e, const char *want,
                            const char *file, int line)
{
    char got[8200];
    size_t n = et_text(e, got, sizeof(got));
    size_t w = strlen(want);
    et_checks++;
    if (n != w || memcmp(got, want, w) != 0) {
        et_fails++;
        printf("FAIL  %s:%d  text is \"%s\" (%zu B), want \"%s\" (%zu B)\n",
               file, line, got, n, want, w);
    }
}
#define ET_IS(e, want) et_is_at((e), (want), __FILE__, __LINE__)

/* 「本文不変」を見るためのスナップショット */
typedef struct { char buf[8200]; size_t len; } et_snap_t;

static inline void et_snap(const edit_t *e, et_snap_t *s)
{
    s->len = et_text(e, s->buf, sizeof(s->buf));
}

static inline void et_snap_same_at(const edit_t *e, const et_snap_t *s,
                                   const char *what, const char *file, int line)
{
    char got[8200];
    size_t n = et_text(e, got, sizeof(got));
    et_checks++;
    if (n != s->len || memcmp(got, s->buf, n) != 0) {
        et_fails++;
        printf("FAIL  %s:%d  %s: text changed (\"%s\" -> \"%s\")\n",
               file, line, what, s->buf, got);
    }
}
#define ET_UNCHANGED(e, s, what) et_snap_same_at((e), (s), (what), __FILE__, __LINE__)

/* --------------------------------------------------------------- カーソル */

static inline void et_cursor_at(const edit_t *e, uint32_t line1, uint32_t col1,
                                size_t byte, const char *file, int line)
{
    edit_pos_t p = edit_cursor(e);
    et_checks++;
    if (p.line1 != line1 || p.col1 != col1 || p.byte != byte) {
        et_fails++;
        printf("FAIL  %s:%d  cursor %u:%u@%zu, want %u:%u@%zu\n", file, line,
               (unsigned)p.line1, (unsigned)p.col1, p.byte,
               (unsigned)line1, (unsigned)col1, byte);
    }
}
#define ET_CURSOR(e, l, c, b) et_cursor_at((e), (l), (c), (b), __FILE__, __LINE__)

/* --------------------------------------------------------------- 1 段の run */

#define ET_MAX_RUNS  512
#define ET_UTF8_CAP  4096

typedef struct {
    int        nruns;
    edit_run_t runs[ET_MAX_RUNS];
    char       utf8[ET_UTF8_CAP];
} et_row_t;

static inline int et_row(const edit_t *e, uint16_t row, et_row_t *r)
{
    memset(r, 0, sizeof(*r));
    r->nruns = edit_view_row(e, row, r->runs, ET_MAX_RUNS,
                             r->utf8, sizeof(r->utf8));
    return r->nruns;
}

/* run を並べた文字列 (utf8_off/utf8_len を run 順に連結) */
static inline size_t et_row_str(const et_row_t *r, char *dst, size_t cap)
{
    size_t n = 0;
    int i;
    if (cap == 0) return 0;
    for (i = 0; i < r->nruns && n + 1 < cap; i++) {
        size_t l = r->runs[i].utf8_len;
        if ((size_t)r->runs[i].utf8_off + l > ET_UTF8_CAP) continue;
        if (n + l + 1 > cap) l = cap - n - 1;
        memcpy(dst + n, r->utf8 + r->runs[i].utf8_off, l);
        n += l;
    }
    dst[n] = 0;
    return n;
}

/* col セルを覆う run の添字。無ければ -1 */
static inline int et_run_at(const et_row_t *r, uint16_t col)
{
    int i;
    for (i = 0; i < r->nruns; i++) {
        if (col >= r->runs[i].col &&
            col < (uint16_t)(r->runs[i].col + r->runs[i].ncells)) {
            return i;
        }
    }
    return -1;
}

/* col セルの字句クラス。run が無ければ EDIT_CLS_N を返す */
static inline unsigned et_cls_at(const et_row_t *r, uint16_t col)
{
    int i = et_run_at(r, col);
    return (i < 0) ? (unsigned)EDIT_CLS_N : (unsigned)r->runs[i].cls;
}

/* run 全部の ncells 合計 */
static inline unsigned et_row_cells(const et_row_t *r)
{
    unsigned n = 0;
    int i;
    for (i = 0; i < r->nruns; i++) n += r->runs[i].ncells;
    return n;
}

/* run の flags の論理和 */
static inline unsigned et_row_flags(const et_row_t *r)
{
    unsigned f = 0;
    int i;
    for (i = 0; i < r->nruns; i++) f |= r->runs[i].flags;
    return f;
}

/* run が col 昇順に隙間なく並び、utf8 の範囲に収まっているか。
   個々のテストが自分で書かなくてよいように共通化してある。 */
static inline void et_row_wellformed_at(const et_row_t *r, const char *file, int line)
{
    unsigned next_col = 0;
    int i;
    if (r->nruns < 0) return;   /* 負は「入らなかった」。呼び出し側が見る */
    for (i = 0; i < r->nruns; i++) {
        const edit_run_t *run = &r->runs[i];
        et_checks++;
        if (run->col != next_col) {
            et_fails++;
            printf("FAIL  %s:%d  run[%d].col=%u, want %u (runs must tile the row "
                   "left to right with no gap and no overlap)\n",
                   file, line, i, (unsigned)run->col, next_col);
        }
        et_checks++;
        if (run->ncells == 0) {
            et_fails++;
            printf("FAIL  %s:%d  run[%d] has ncells==0\n", file, line, i);
        }
        et_checks++;
        if ((size_t)run->utf8_off + run->utf8_len > ET_UTF8_CAP) {
            et_fails++;
            printf("FAIL  %s:%d  run[%d] utf8 range %u+%u is outside the buffer\n",
                   file, line, i, (unsigned)run->utf8_off, (unsigned)run->utf8_len);
        }
        et_checks++;
        if (run->cls >= EDIT_CLS_N) {
            et_fails++;
            printf("FAIL  %s:%d  run[%d].cls=%u out of range\n",
                   file, line, i, (unsigned)run->cls);
        }
        next_col = (unsigned)run->col + run->ncells;
    }
}
#define ET_ROW_WELLFORMED(r) et_row_wellformed_at((r), __FILE__, __LINE__)

/* UTF-8 の先頭バイトを数える (継続バイト 10xxxxxx を除く) */
static inline unsigned et_cp_count(const char *s, size_t len)
{
    unsigned n = 0;
    size_t i;
    for (i = 0; i < len; i++) {
        if (((unsigned char)s[i] & 0xC0) != 0x80) n++;
    }
    return n;
}

/* --------------------------------------------------------------- 不変条件 */

/*
 * edit_check() は「打鍵経路では呼ばない」検査 (§A.1)。ホストは毎 op 呼ぶ
 * のが M3 のゲート (§C.6) なので、op のたびにこれを通す。
 */
static inline void et_ok_at(const edit_t *e, const char *what,
                            const char *file, int line)
{
    et_checks++;
    if (!edit_check(e)) {
        et_fails++;
        printf("FAIL  %s:%d  edit_check() false after %s\n", file, line, what);
    }
}
#define ET_OK(e, what) et_ok_at((e), (what), __FILE__, __LINE__)

/* ----------------------------------------------------------- UTF-8 の不正列 */

typedef struct { const char *name; const char *bytes; size_t len; } et_bad_utf8_t;

/*
 * §A.1 EDIT_E_UTF8 の対象。RFC 3629 が禁じる 4 系統を 1 つずつ:
 * 孤立継続バイト / 過剰長 / サロゲート / 途中で切れた列。
 * 加えて範囲外の先頭バイト (F5.. と FE/FF) と 4 バイト超の列。
 */
static inline const et_bad_utf8_t *et_bad_utf8(size_t *count)
{
    static const et_bad_utf8_t v[] = {
        { "lone continuation 0x80",      "\x80",                 1 },
        { "lone continuation 0xBF",      "\xBF",                 1 },
        { "continuation after ASCII",    "a\x80",                2 },
        { "overlong 2-byte slash",       "\xC0\xAF",             2 },
        { "overlong 2-byte NUL",         "\xC0\x80",             2 },
        { "overlong 3-byte slash",       "\xE0\x80\xAF",         3 },
        { "overlong 4-byte slash",       "\xF0\x80\x80\xAF",     4 },
        { "surrogate U+D800",            "\xED\xA0\x80",         3 },
        { "surrogate U+DFFF",            "\xED\xBF\xBF",         3 },
        { "truncated 3-byte lead",       "\xE6\x97",             2 },
        { "truncated 2-byte lead",       "\xC3",                 1 },
        { "truncated 4-byte lead",       "\xF0\x9F\x98",         3 },
        { "lead F5 (beyond U+10FFFF)",   "\xF5\x80\x80\x80",     4 },
        { "lead FE",                     "\xFE",                 1 },
        { "lead FF",                     "\xFF",                 1 },
        { "5-byte sequence",             "\xF8\x88\x80\x80\x80", 5 },
        { "valid head then bad tail",    "ok\xE6\x97",           4 },
    };
    *count = sizeof(v) / sizeof(v[0]);
    return v;
}

/* ------------------------------------------------- 毎 op の edit_check モード

   §C.6 の M3 ゲートは「edit_check 毎 op で緑」。ET_OK は各テストが置いた
   場所でしか呼ばれないので、それだけでは「毎 op」にならない。
   -DEDIT_TEST_CHECK_EVERY_OP を付けると、状態を動かす公開関数の呼び出しが
   すべて呼び出し直後に edit_check() を通る (run_tests.sh --every-op)。

   仕組み: 関数名と同名の関数マクロで包み、中では `(name)(...)` と括弧で
   括ってマクロ展開を止めて本物を呼ぶ。e が NULL の呼び出し (E_ARG の経路を
   見るテスト) は edit_check の対象外なので素通しする。
   e は 1 回の呼び出しで 2 回評価されるので、副作用のある式を渡さないこと
   (テストはどれも `ed.e` のような単純な左辺値を渡している)。

   既定は off。理由は費用ではなく切り分け: 毎 op モードで初めて赤くなった
   ときに「どの op で壊れたか」を見るために、既定の実行と比べられる方がよい。
 */
#ifdef EDIT_TEST_CHECK_EVERY_OP

static inline void et_hook_at(const edit_t *e, const char *what,
                              const char *file, int line)
{
    if (e == NULL)
        return;                 /* NULL 引数のテストは edit_check の対象外 */
    et_checks++;
    if (!edit_check(e)) {
        et_fails++;
        printf("FAIL  %s:%d  edit_check() false right after %s\n",
               file, line, what);
    }
}

/* 返り値の型ごとに 3 つ。void はコンマ式、それ以外は値を素通しする。 */
static inline edit_err_t et_hook_e(const edit_t *e, edit_err_t r, const char *what,
                                   const char *file, int line)
{
    et_hook_at(e, what, file, line);
    return r;
}

static inline int et_hook_i(const edit_t *e, int r, const char *what,
                            const char *file, int line)
{
    et_hook_at(e, what, file, line);
    return r;
}

#define ET_HOOK_E(fn, e, ...) \
    et_hook_e((e), (fn)((e), __VA_ARGS__), #fn, __FILE__, __LINE__)
#define ET_HOOK_V(fn, e, ...) \
    ((fn)((e), __VA_ARGS__), et_hook_at((e), #fn, __FILE__, __LINE__))
#define ET_HOOK_V0(fn, e) \
    ((fn)((e)), et_hook_at((e), #fn, __FILE__, __LINE__))

#define edit_set_text(e, ...)         ET_HOOK_E(edit_set_text, (e), __VA_ARGS__)
#define edit_insert(e, ...)           ET_HOOK_E(edit_insert, (e), __VA_ARGS__)
#define edit_delete(e, ...)           ET_HOOK_E(edit_delete, (e), __VA_ARGS__)
#define edit_undo(e)                  et_hook_e((e), (edit_undo)((e)), "edit_undo", __FILE__, __LINE__)
#define edit_redo(e)                  et_hook_e((e), (edit_redo)((e)), "edit_redo", __FILE__, __LINE__)
#define edit_move(e, ...)             ET_HOOK_E(edit_move, (e), __VA_ARGS__)
#define edit_goto(e, ...)             ET_HOOK_E(edit_goto, (e), __VA_ARGS__)
#define edit_delete_selection(e)      et_hook_e((e), (edit_delete_selection)((e)), "edit_delete_selection", __FILE__, __LINE__)
#define edit_set_preedit(e, ...)      ET_HOOK_E(edit_set_preedit, (e), __VA_ARGS__)
#define edit_set_mark(e, ...)         ET_HOOK_E(edit_set_mark, (e), __VA_ARGS__)
#define edit_scroll(e, ...)           ET_HOOK_E(edit_scroll, (e), __VA_ARGS__)
#define edit_view_row(e, ...)         et_hook_i((e), (edit_view_row)((e), __VA_ARGS__), "edit_view_row", __FILE__, __LINE__)
#define edit_select_begin(e)          ET_HOOK_V0(edit_select_begin, (e))
#define edit_select_end(e)            ET_HOOK_V0(edit_select_end, (e))
#define edit_clear_mark(e)            ET_HOOK_V0(edit_clear_mark, (e))
#define edit_mark_saved(e)            ET_HOOK_V0(edit_mark_saved, (e))
#define edit_dirty_clear(e)           ET_HOOK_V0(edit_dirty_clear, (e))
#define edit_set_view(e, ...)         ET_HOOK_V(edit_set_view, (e), __VA_ARGS__)

#endif /* EDIT_TEST_CHECK_EVERY_OP */

#endif /* EDIT_TEST_H */
