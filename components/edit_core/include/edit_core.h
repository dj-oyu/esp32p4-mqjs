/*
 * edit_core — the editor model: gap buffer, line index, undo, per-line lexer,
 * and the row/run view the presenter blits.  Pure C99: no ESP-IDF headers, no
 * malloc, no static mutable state, no locks.  One caller-supplied block holds
 * everything (see edit_mem_size), so the whole component can be exercised on
 * the host under ASAN/UBSAN and by libFuzzer.
 *
 * The declarations below are docs/native-editor-spec.md §A.1 verbatim — the
 * presenter, the host tests and this implementation are all written against
 * that block, so nothing here may be renamed or reordered.  Comments are the
 * only thing added; where behaviour was not pinned down by the spec it is
 * stated in the file-level notes of the implementation, not silently decided
 * in a signature.
 *
 * Rules the implementation keeps (spec §A.1):
 *   - no public function hands back a pointer into the core.  The gap moves,
 *     so a presenter holding a char* would be holding a dangling one.
 *   - a function that changes the text and fails changes nothing: E_FULL /
 *     E_LINES / E_UTF8 are decided before the first byte is written.
 *   - what can be clamped is clamped and returns EDIT_OK; the caller is never
 *     asked to work out that it hit an edge.
 *
 * Units, since three different ones appear in this header:
 *   - `byte`  : offset into the text, gap excluded (what edit_copy_text takes).
 *   - `col1`  : 1-based *cell* column — a CJK character is two cells, a tab is
 *               as many cells as it takes to reach the next tab stop.
 *   - `row`   : 0-based screen row.  No soft wrap, so one row is one line.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 上限 (呼び出し側が init で決める。既定は MQJS_SCRIPT_MAX に揃える) ---- */
typedef struct {
    uint32_t max_bytes;   /* 本文の上限。既定 128*1024 (= MQJS_SCRIPT_MAX) */
    uint32_t max_lines;   /* 行インデックスの上限。既定 8192 */
    uint32_t undo_bytes;  /* undo リング。既定 64*1024 */
    uint16_t max_cols, max_rows;  /* 表示の最大。既定 142 / 49 (両向きの最大) */
} edit_config_t;

typedef struct edit edit_t;                       /* 不透明。ブロックの先頭に置かれる */

typedef enum {
    EDIT_OK = 0,
    EDIT_E_ARG,     /* NULL / 範囲外の引数 */
    EDIT_E_FULL,    /* max_bytes 超過。本文は変更されていない */
    EDIT_E_LINES,   /* max_lines 超過。同上 */
    EDIT_E_UTF8,    /* 不正な UTF-8。同上 */
    EDIT_E_STATE,   /* その状態では不可 (選択が無いのに delete_selection 等) */
} edit_err_t;

size_t     edit_mem_size(const edit_config_t *cfg);
edit_err_t edit_init(void *block, size_t block_len, const edit_config_t *cfg,
                     edit_t **out);              /* block は 8B 整列、寿命は core と同じ */

/* ---- 本文の出し入れ (バイトが境界を越える唯一の場所) ---- */
edit_err_t edit_set_text(edit_t *e, const char *utf8, size_t len);   /* 置換、undo 消去、カーソル 0 */
size_t     edit_text_len(const edit_t *e);
size_t     edit_copy_text(const edit_t *e, size_t off, char *dst, size_t cap);
           /* off から cap まで複製。保存 (チャンク) と構文チェックの入力用。gap を跨いでも連続 */

/* ---- 編集 (全部カーソル位置、全部 undo 可) ---- */
edit_err_t edit_insert(edit_t *e, const char *utf8, size_t len);     /* 選択があれば置換 */
edit_err_t edit_delete(edit_t *e, int32_t n);   /* n<0: backspace |n| 文字、n>0: 前方 n 文字。端で clamp、OK */
edit_err_t edit_undo(edit_t *e);                /* 何も無ければ EDIT_E_STATE */
edit_err_t edit_redo(edit_t *e);

/* ---- カーソル・選択 ---- */
typedef enum { EDIT_M_LEFT, EDIT_M_RIGHT, EDIT_M_UP, EDIT_M_DOWN, EDIT_M_HOME, EDIT_M_END,
               EDIT_M_PGUP, EDIT_M_PGDN, EDIT_M_DOC_HOME, EDIT_M_DOC_END,
               EDIT_M_WORD_LEFT, EDIT_M_WORD_RIGHT } edit_motion_t;
edit_err_t edit_move(edit_t *e, edit_motion_t m, uint32_t n);         /* 端で clamp、OK */
edit_err_t edit_goto(edit_t *e, uint32_t line1, uint32_t col1);        /* 1 始まり (JS のエラー位置と同じ)。clamp、OK。line1==0 は E_ARG */
void       edit_select_begin(edit_t *e);        /* アンカー = カーソル */
void       edit_select_end(edit_t *e);          /* 選択解除 */
bool       edit_selection(const edit_t *e, size_t *from_byte, size_t *to_byte);
size_t     edit_copy_selection(const edit_t *e, char *dst, size_t cap);
edit_err_t edit_delete_selection(edit_t *e);

typedef struct { uint32_t line1, col1; size_t byte; } edit_pos_t;    /* col1 はセル列 (CJK=2) */
edit_pos_t edit_cursor(const edit_t *e);
uint32_t   edit_line_count(const edit_t *e);

/* ---- IME の合成文字列 (バッファには入れない。カーソル位置に重ねて描くだけ) ---- */
edit_err_t edit_set_preedit(edit_t *e, const char *utf8, size_t len);  /* len 0 = 消す。上限 128 B */

/* ---- エラー位置のマーク (実行時エラーの行を色で示す) ---- */
edit_err_t edit_set_mark(edit_t *e, uint32_t line1, uint32_t col1);
void       edit_clear_mark(edit_t *e);

/* ---- 表示 ---- */
void       edit_set_view(edit_t *e, uint16_t cols, uint16_t rows);   /* 回転・キーボード表示で呼ぶ。全行 dirty */
edit_err_t edit_scroll(edit_t *e, int32_t lines);                    /* タッチのドラッグ用。カーソルは動かさない */
/* カーソルが画面外へ出る操作は core が自動で追従スクロールする (ソフトラップ無し、横も同様) */

/* 何を描き直すか。行 = 画面の段 (ソフトラップが無いので 1 対 1) */
#define EDIT_DIRTY_ALL     (1u << 0)   /* スクロール・set_view・set_text の後 */
#define EDIT_DIRTY_CURSOR  (1u << 1)   /* カーソル行が変わった (旧行も rows に入っている) */
#define EDIT_DIRTY_STATUS  (1u << 2)   /* 行数・変更フラグ・位置 */
uint32_t   edit_dirty_flags(const edit_t *e);
uint64_t   edit_dirty_rows(const edit_t *e);    /* bit i = 画面 i 段目。rows ≤ 49 < 64 */
void       edit_dirty_clear(edit_t *e);

/* 1 段ぶんを「同じ色の連続 (run)」に切って返す。プレゼンタは run ごとに
   1 回 cells blit を呼ぶ。utf8 は呼び出し側のバッファに複製される —— core の
   内側を指すポインタは出さない。返り値は run 数、utf8_cap 不足なら負。 */
typedef enum { EDIT_CLS_PLAIN = 0, EDIT_CLS_KEYWORD, EDIT_CLS_IDENT, EDIT_CLS_NUMBER,
               EDIT_CLS_STRING, EDIT_CLS_COMMENT, EDIT_CLS_PUNCT, EDIT_CLS_PREEDIT,
               EDIT_CLS_N } edit_cls_t;
#define EDIT_RUN_SELECTED (1u << 0)
#define EDIT_RUN_CURSOR   (1u << 1)   /* この run の先頭セルがカーソル */
#define EDIT_RUN_MARK     (1u << 2)   /* エラー位置 */
typedef struct {
    uint16_t col, ncells;             /* 画面上の列と幅 (CJK は filler 込み、ui.cells の CONT 契約どおり) */
    uint8_t  cls, flags;
    uint16_t utf8_off, utf8_len;      /* utf8 バッファ内の位置 */
} edit_run_t;
int        edit_view_row(const edit_t *e, uint16_t row, edit_run_t *runs, int runs_cap,
                         char *utf8, size_t utf8_cap);
bool       edit_cursor_view(const edit_t *e, uint16_t *row, uint16_t *col);  /* 画面外なら false */

/* ---- 状態 ---- */
bool       edit_modified(const edit_t *e);
void       edit_mark_saved(edit_t *e);
uint32_t   edit_edit_count(const edit_t *e);    /* オートセーブの「100 編集」用。単調増加 */

/* ---- 検査と統計 (打鍵経路では呼ばない) ---- */
bool       edit_check(const edit_t *e);         /* 全不変条件を再計数で検査。ホストは毎 op、実機は Kconfig */
typedef struct {
    uint32_t gap_moves, gap_bytes_moved;        /* Q3 #2 の閾値 (piece table へ倒す判断) の材料 */
    uint32_t relex_lines, relex_cascades;       /* コメント開始で後続へ波及した回数 */
    uint32_t newline_index_moves, undo_evictions;
} edit_stats_t;
void       edit_stats(const edit_t *e, edit_stats_t *out);

#ifdef __cplusplus
}
#endif
