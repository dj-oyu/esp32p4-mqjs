/*
 * fuzz_edit.c — libFuzzer harness。入力バイト列を op 列として解釈し、
 * **毎 op のあとに edit_check() を呼ぶ**。
 *
 *   ./run_tests.sh --fuzz 60        # 60 秒
 *   wsl clang -fsanitize=fuzzer,address,undefined -std=c99 \
 *       -I.. -I. fuzz_edit.c ../<all edit_core .c> -o build/fuzz_edit
 *
 * ここで固定する仕様 (docs/native-editor-spec.md §A.1):
 *  - 「不正な UTF-8 -> EDIT_E_UTF8」。断片を insert に食わせて、
 *    **弾かれるか、通ったなら本文が壊れていないか**を見る
 *  - 上限 (max_bytes / max_lines) をどんな順序でも越えない
 *  - 不変条件 6 つ (edit_check)
 *  - 「core の内側を指すポインタは出さない」= 返ってきた run の utf8_off/len が
 *    呼び出し側バッファの中に収まる (ASAN が越境を捕まえる)
 *
 * ---------------------------------------------------------------------------
 * この harness が見ていないもの
 *
 *  - 返り値そのもの。任意のバイト列に対して「どのエラーが正しいか」を
 *    ここで決めることはできないので、エラーコードは一切 assert しない。
 *    見ているのは**不変条件と上限**だけ
 *  - 表示の内容 (何が描かれるか)。run が整合しているかまで
 *  - edit_check() が本当に壊れを見つけられるか。それは敵対的検証の担当で、
 *    ここが緑でも edit_check が `return true;` なら意味が無い
 *  - 実機のメモリ量。ホストの config は小さめ (max_bytes 4 KB) にして
 *    1 実行あたりの時間を稼いでいるので、128 KB 固有の桁溢れは出ない
 *  - マルチスレッド
 * ---------------------------------------------------------------------------
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "edit_core.h"

#define FZ_MAX_BYTES  4096u
#define FZ_MAX_LINES  64u
#define FZ_UNDO_BYTES 512u
#define FZ_MAX_COLS   40u
#define FZ_MAX_ROWS   8u

static void die(const char *what, const edit_t *e)
{
    fprintf(stderr, "fuzz: %s (text_len=%zu line_count=%u cursor=%zu)\n",
            what, edit_text_len(e), (unsigned)edit_line_count(e),
            edit_cursor(e).byte);
    abort();
}

static void audit(edit_t *e, const char *after)
{
    edit_pos_t p;

    if (!edit_check(e)) die(after, e);
    if (edit_text_len(e) > FZ_MAX_BYTES) die("text_len over max_bytes", e);
    if (edit_line_count(e) > FZ_MAX_LINES) die("line_count over max_lines", e);
    if (edit_line_count(e) == 0) die("line_count is zero", e);
    p = edit_cursor(e);
    if (p.byte > edit_text_len(e)) die("cursor past the end", e);
    if (p.line1 == 0 || p.line1 > edit_line_count(e)) die("cursor line out of range", e);
    if (p.col1 == 0) die("cursor col1 is zero (columns are 1-based)", e);
    (void)after;
}

/* 選択範囲が本文の中に収まり、from <= to であること */
static void audit_selection(edit_t *e)
{
    size_t from = 0, to = 0;
    if (!edit_selection(e, &from, &to)) return;
    if (from > to) die("selection from > to", e);
    if (to > edit_text_len(e)) die("selection past the end", e);
}

/* 1 段を引いて run の整合を見る */
static void audit_row(edit_t *e, uint16_t row)
{
    edit_run_t runs[128];
    char utf8[512];
    int n, i;
    unsigned next_col = 0;

    memset(runs, 0, sizeof(runs));
    memset(utf8, 0, sizeof(utf8));
    n = edit_view_row(e, row, runs, (int)(sizeof(runs) / sizeof(runs[0])),
                      utf8, sizeof(utf8));
    if (n < 0) return;                       /* バッファ不足は正常な返り */
    if (n > (int)(sizeof(runs) / sizeof(runs[0]))) die("view_row returned too many runs", e);
    for (i = 0; i < n; i++) {
        if (runs[i].col != next_col) die("runs do not tile the row", e);
        if (runs[i].ncells == 0) die("run with ncells == 0", e);
        if (runs[i].cls >= EDIT_CLS_N) die("run cls out of range", e);
        if ((size_t)runs[i].utf8_off + runs[i].utf8_len > sizeof(utf8)) {
            die("run utf8 range outside the caller buffer", e);
        }
        next_col = (unsigned)runs[i].col + runs[i].ncells;
        if (next_col > 0xFFFFu) die("run columns overflowed", e);
    }
}

/* 本文を丸ごと取り出して、返り値ぶんだけ書かれていること */
static void audit_copy(edit_t *e)
{
    static char buf[FZ_MAX_BYTES + 16];
    size_t len = edit_text_len(e);
    size_t n;

    memset(buf, 0x5A, sizeof(buf));
    n = edit_copy_text(e, 0, buf, len);
    if (n != len) die("copy_text(0, len) returned a short count", e);
    if (buf[len] != 0x5A) die("copy_text wrote past the returned length", e);

    if (len > 1) {
        size_t off = len / 2;
        memset(buf, 0x5A, sizeof(buf));
        n = edit_copy_text(e, off, buf, len);
        if (n != len - off) die("copy_text(off, len) returned the wrong count", e);
        if (buf[n] != 0x5A) die("copy_text wrote past the returned length", e);
    }
    /* 範囲外の off は 0 バイト */
    memset(buf, 0x5A, sizeof(buf));
    if (edit_copy_text(e, len + 1, buf, 16) != 0) {
        die("copy_text past the end returned non-zero", e);
    }
    if (buf[0] != 0x5A) die("copy_text past the end wrote into the buffer", e);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static void *block;
    static size_t block_len;
    edit_config_t cfg;
    edit_t *e = NULL;
    size_t p = 0;
    int ops = 0;

    cfg.max_bytes  = FZ_MAX_BYTES;
    cfg.max_lines  = FZ_MAX_LINES;
    cfg.undo_bytes = FZ_UNDO_BYTES;
    cfg.max_cols   = (uint16_t)FZ_MAX_COLS;
    cfg.max_rows   = (uint16_t)FZ_MAX_ROWS;

    if (!block) {
        block_len = edit_mem_size(&cfg);
        if (block_len == 0) return 0;
        block = malloc(block_len);
        if (!block) return 0;
    }
    memset(block, 0xA5, block_len);
    if (edit_init(block, block_len, &cfg, &e) != EDIT_OK || !e) return 0;
    edit_set_view(e, (uint16_t)FZ_MAX_COLS, (uint16_t)FZ_MAX_ROWS);
    audit(e, "init");

    while (p < size && ops < 4096) {
        uint8_t op = data[p++] & 0x0F;
        ops++;

        switch (op) {
        case 0: {   /* insert — 任意のバイト列。不正な UTF-8 が主役 */
            size_t n = (p < size) ? (data[p++] % 24u) : 0;
            if (n > size - p) n = size - p;
            (void)edit_insert(e, (const char *)(data + p), n);
            p += n;
            break;
        }
        case 1: {   /* set_text — 同じく任意のバイト列 */
            size_t n = (p < size) ? (data[p++] % 64u) : 0;
            if (n > size - p) n = size - p;
            (void)edit_set_text(e, (const char *)(data + p), n);
            p += n;
            break;
        }
        case 2: {   /* delete */
            int32_t n = (p < size) ? (int32_t)(int8_t)data[p++] : 1;
            (void)edit_delete(e, n);
            break;
        }
        case 3: {   /* move */
            uint8_t m = (p < size) ? (uint8_t)(data[p++] % 12u) : 0;
            uint32_t k = (p < size) ? (uint32_t)(data[p++] % 5u) : 1;
            (void)edit_move(e, (edit_motion_t)m, k);
            break;
        }
        case 4:
            (void)edit_undo(e);
            break;
        case 5:
            (void)edit_redo(e);
            break;
        case 6:
            edit_select_begin(e);
            break;
        case 7:
            edit_select_end(e);
            break;
        case 8:
            (void)edit_delete_selection(e);
            break;
        case 9: {   /* goto — line1 に 0 も混ぜる (E_ARG の経路) */
            uint32_t l = (p < size) ? (uint32_t)(data[p++] % 70u) : 1;
            uint32_t c = (p < size) ? (uint32_t)(data[p++] % 50u) : 1;
            (void)edit_goto(e, l, c);
            break;
        }
        case 10: { /* set_view */
            uint16_t cols = (uint16_t)(1u + ((p < size) ? data[p++] % FZ_MAX_COLS : 0));
            uint16_t rows = (uint16_t)(1u + ((p < size) ? data[p++] % FZ_MAX_ROWS : 0));
            edit_set_view(e, cols, rows);
            break;
        }
        case 11: { /* view_row — 画面外の段も引く */
            uint16_t row = (uint16_t)((p < size) ? data[p++] % (FZ_MAX_ROWS + 3u) : 0);
            audit_row(e, row);
            break;
        }
        case 12: { /* set_preedit — 上限 128 B を跨がせる */
            size_t n = (p < size) ? (data[p++] % 160u) : 0;
            if (n > size - p) n = size - p;
            (void)edit_set_preedit(e, (const char *)(data + p), n);
            p += n;
            break;
        }
        case 13: { /* scroll */
            int32_t n = (p < size) ? (int32_t)(int8_t)data[p++] : 0;
            (void)edit_scroll(e, n);
            break;
        }
        case 14: { /* mark */
            uint32_t l = (p < size) ? (uint32_t)(data[p++] % 70u) : 1;
            if (l == 0) edit_clear_mark(e);
            else (void)edit_set_mark(e, l, 1);
            break;
        }
        default: { /* 読み出し系をまとめて */
            uint16_t crow = 0, ccol = 0;
            edit_stats_t st;
            char sel[256];
            (void)edit_cursor_view(e, &crow, &ccol);
            (void)edit_dirty_flags(e);
            (void)edit_dirty_rows(e);
            edit_dirty_clear(e);
            (void)edit_modified(e);
            (void)edit_edit_count(e);
            memset(&st, 0, sizeof(st));
            edit_stats(e, &st);
            (void)edit_copy_selection(e, sel, sizeof(sel));
            audit_copy(e);
            break;
        }
        }

        audit(e, "op");
        audit_selection(e);
    }

    /* 最後にひととおり読む: 遅延の再字句をここで解かせる */
    {
        uint16_t r;
        for (r = 0; r < FZ_MAX_ROWS; r++) audit_row(e, r);
    }
    audit(e, "final");
    audit_copy(e);
    return 0;
}

#ifdef EDIT_FUZZ_STANDALONE
/*
 * libFuzzer 無しでも構文と 1 件の再生を確かめられるようにしておく。
 * 引数のファイルを 1 つずつ食わせるだけ。
 */
int main(int argc, char **argv)
{
    int i;
    for (i = 1; i < argc; i++) {
        static uint8_t buf[1 << 16];
        size_t n;
        FILE *f = fopen(argv[i], "rb");
        if (!f) { perror(argv[i]); return 1; }
        n = fread(buf, 1, sizeof(buf), f);
        fclose(f);
        (void)LLVMFuzzerTestOneInput(buf, n);
        printf("ok %s (%zu bytes)\n", argv[i], n);
    }
    if (argc == 1) {
        static const uint8_t seed[] = {
            0x00, 0x05, 'h', 'e', 'l', 'l', 'o',
            0x03, 0x01, 0x01,
            0x00, 0x02, 0xE6, 0x97,
            0x0B, 0x00,
            0x04, 0x05,
            0x0F,
        };
        (void)LLVMFuzzerTestOneInput(seed, sizeof(seed));
        printf("ok built-in seed\n");
    }
    return 0;
}
#endif
