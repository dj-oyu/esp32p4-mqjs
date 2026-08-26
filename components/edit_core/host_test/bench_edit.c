/*
 * bench_edit.c — docs/native-editor-spec.md §C.3 の 5 種をホストで回す。
 *
 * ============================================================================
 * ホストの数値は**回帰検出用**であって、実機 (ESP32-P4 / 360 MHz / PSRAM) の
 * 値ではない。x86 の L2 に載った 300 KB と、PSRAM 上の 300 KB は別物である。
 * ここの数字を仕様や設計文書に「実測」として書かないこと。実機の値は
 * §C.1/§C.3 の経路で、実機でしか取れない (§F #8/#10)。
 *
 * ここで見るのは「前より遅くなっていないか」だけ。桁が動いたら、
 * データ構造をいじった側が説明する。
 * ============================================================================
 *
 *   ./run_tests.sh --bench
 *
 * §C.3 が名指しする 5 種:
 *   1. 1 文字挿入          — 打鍵 1 回ぶん
 *   2. 改行挿入            — 行索引の末尾 memmove が動く
 *   3. 300 KB での gap 移動 — Q3 #2 (piece table へ倒すか) の材料
 *   4. コメント開始の波及   — 1 打鍵で何行ぶん再字句するか
 *   5. view_row 142 列     — 横向き 1 段ぶん (§0 の 142 列 × 26 行)
 *
 * ---------------------------------------------------------------------------
 * このベンチが見ていないもの
 *
 *  - キャッシュの効き方。ホストは全部 L2 に載る
 *  - 実機の PPA / blit。ここは edit_core の中だけ
 *  - 最悪値の分布。各項目は平均しか出さない (p99 は実機の perf_meter の仕事)
 *  - メモリ使用量。edit_mem_size は表示するが評価しない
 * ---------------------------------------------------------------------------
 */
/* clock_gettime / CLOCK_MONOTONIC は POSIX。-std=c99 だけだと glibc が
   宣言を隠すので、include より前にこれが要る。 */
#define _POSIX_C_SOURCE 199309L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#include "edit_core.h"

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void report(const char *name, double secs, long iters, const char *unit)
{
    double per = iters ? secs / (double)iters : 0.0;
    printf("%-28s %9ld %-10s %10.3f us each   (%.3f s total)\n",
           name, iters, unit, per * 1e6, secs);
}

static void *open_edit(const edit_config_t *cfg, edit_t **out)
{
    size_t n = edit_mem_size(cfg);
    void *block = malloc(n);
    if (!block) {
        fprintf(stderr, "bench: malloc(%zu) failed\n", n);
        exit(1);
    }
    if (edit_init(block, n, cfg, out) != EDIT_OK || !*out) {
        fprintf(stderr, "bench: edit_init failed\n");
        exit(1);
    }
    printf("note: edit_mem_size(max_bytes=%u, max_lines=%u, undo=%u) = %zu B\n",
           (unsigned)cfg->max_bytes, (unsigned)cfg->max_lines,
           (unsigned)cfg->undo_bytes, n);
    return block;
}

static edit_config_t cfg_small(void)
{
    edit_config_t c;
    c.max_bytes = 128u * 1024u;      /* MQJS_SCRIPT_MAX (§E #10) */
    c.max_lines = 8192;
    c.undo_bytes = 64u * 1024u;      /* §E #13 */
    c.max_cols = 142;                /* §0 の横向き */
    c.max_rows = 26;
    return c;
}

/* ------------------------------------------------------------ 1. 1 文字挿入 */

static void b_insert_char(void)
{
    edit_config_t cfg = cfg_small();
    edit_t *e;
    void *block = open_edit(&cfg, &e);
    long i, iters = 100000;
    double t0;

    edit_set_view(e, 142, 26);
    edit_set_text(e, "", 0);

    t0 = now_s();
    for (i = 0; i < iters; i++) {
        if (edit_insert(e, "x", 1) != EDIT_OK) {
            /* 上限に当たったら畳んで続ける (時間は計り続ける — これも実装の
               コストの一部だが、上限を大きく取ってあるので普通は起きない) */
            edit_set_text(e, "", 0);
        }
    }
    report("insert 1 char", now_s() - t0, iters, "inserts");
    printf("     text_len=%zu line_count=%u\n",
           edit_text_len(e), (unsigned)edit_line_count(e));
    free(block);
}

/* -------------------------------------------------------------- 2. 改行挿入 */

static void b_insert_newline(void)
{
    edit_config_t cfg = cfg_small();
    edit_t *e;
    void *block = open_edit(&cfg, &e);
    long i, iters = 8000;
    double t0;

    edit_set_view(e, 142, 26);
    edit_set_text(e, "", 0);

    /* 行索引の末尾 memmove を効かせるため、常に**先頭**へ改行を挿す。
       末尾に足すだけだと memmove が 0 バイトになる。 */
    t0 = now_s();
    for (i = 0; i < iters; i++) {
        edit_move(e, EDIT_M_DOC_HOME, 1);
        if (edit_insert(e, "\n", 1) != EDIT_OK) break;
    }
    report("insert newline at head", now_s() - t0, i, "inserts");
    printf("     line_count=%u\n", (unsigned)edit_line_count(e));
    free(block);
}

/* -------------------------------------------------------- 3. 300 KB gap 移動 */

static void b_gap_move(void)
{
    edit_config_t cfg = cfg_small();
    edit_t *e;
    void *block;
    char *seed;
    const size_t N = 300u * 1024u;
    long i, iters = 200;
    double t0;
    edit_stats_t st;

    cfg.max_bytes = (uint32_t)(N + 1024u);
    cfg.max_lines = 8192;
    block = open_edit(&cfg, &e);

    seed = (char *)malloc(N);
    if (!seed) { fprintf(stderr, "bench: malloc failed\n"); exit(1); }
    for (i = 0; i < (long)N; i++) {
        seed[i] = (i % 64 == 63) ? '\n' : (char)('a' + (i % 26));
    }
    if (edit_set_text(e, seed, N) != EDIT_OK) {
        fprintf(stderr, "bench: set_text(300 KB) failed\n");
        exit(1);
    }
    edit_set_view(e, 142, 26);

    /* 端から端へ往復させる。gap buffer なら 1 往復で 2×300 KB 動く。 */
    memset(&st, 0, sizeof(st));
    edit_stats(e, &st);
    printf("note: before: gap_moves=%u gap_bytes_moved=%u\n",
           (unsigned)st.gap_moves, (unsigned)st.gap_bytes_moved);

    t0 = now_s();
    for (i = 0; i < iters; i++) {
        edit_move(e, EDIT_M_DOC_END, 1);
        edit_insert(e, "z", 1);
        edit_move(e, EDIT_M_DOC_HOME, 1);
        edit_insert(e, "a", 1);
    }
    {
        double secs = now_s() - t0;
        report("gap sweep over 300 KB", secs, iters * 2, "sweeps");
        memset(&st, 0, sizeof(st));
        edit_stats(e, &st);
        printf("note: after:  gap_moves=%u gap_bytes_moved=%u  (%.1f MB/s "
               "of gap traffic)\n",
               (unsigned)st.gap_moves, (unsigned)st.gap_bytes_moved,
               secs > 0 ? (double)st.gap_bytes_moved / secs / 1e6 : 0.0);
    }
    free(seed);
    free(block);
}

/* ------------------------------------------------- 4. コメント開始の波及 */

static void b_comment_cascade(void)
{
    edit_config_t cfg = cfg_small();
    edit_t *e;
    void *block = open_edit(&cfg, &e);
    char *doc;
    size_t n = 0;
    int i;
    const int LINES = 2000;
    long iters = 2000;
    double t0;
    edit_stats_t before, after;

    doc = (char *)malloc((size_t)LINES * 16u);
    if (!doc) { fprintf(stderr, "bench: malloc failed\n"); exit(1); }
    for (i = 0; i < LINES; i++) n += (size_t)sprintf(doc + n, "foo(%03d);\n", i);

    edit_set_view(e, 142, 26);
    if (edit_set_text(e, doc, n) != EDIT_OK) {
        fprintf(stderr, "bench: set_text failed (line_count over max_lines?)\n");
        exit(1);
    }

    memset(&before, 0, sizeof(before));
    edit_stats(e, &before);

    /* 先頭でブロックコメントを開いて閉じる。開いた瞬間、可視域の下端まで
       波及するはず (§A.1)。閉じるとまた波及する。 */
    t0 = now_s();
    for (i = 0; i < (int)iters; i++) {
        edit_goto(e, 1, 1);
        edit_insert(e, "/*", 2);
        edit_delete(e, -2);
    }
    {
        double secs = now_s() - t0;
        report("open+close block comment", secs, iters, "pairs");
        memset(&after, 0, sizeof(after));
        edit_stats(e, &after);
        printf("note: relex_lines +%u, relex_cascades +%u over %ld pairs "
               "(%d-line document, 26-row view)\n",
               (unsigned)(after.relex_lines - before.relex_lines),
               (unsigned)(after.relex_cascades - before.relex_cascades),
               iters, LINES);
    }
    free(doc);
    free(block);
}

/* ---------------------------------------------------------- 5. view_row 142 */

static void b_view_row(void)
{
    edit_config_t cfg = cfg_small();
    edit_t *e;
    void *block = open_edit(&cfg, &e);
    char line[256];
    char doc[4096];
    size_t n = 0;
    int i;
    long iters = 200000;
    double t0;
    edit_run_t runs[256];
    char utf8[1024];
    long total_runs = 0;

    /* 142 セルぶん。色が変わるものを混ぜて run を実際に切らせる */
    while (n < 142) {
        n += (size_t)sprintf(line + n, "var a%02d = %d; ",
                             (int)(n % 100), (int)(n % 10));
    }
    line[142] = 0;

    n = 0;
    for (i = 0; i < 26; i++) n += (size_t)sprintf(doc + n, "%s\n", line);
    edit_set_view(e, 142, 26);
    edit_set_text(e, doc, n);

    /* 1 回引いて遅延の再字句を済ませておく (測るのは定常状態) */
    for (i = 0; i < 26; i++) {
        (void)edit_view_row(e, (uint16_t)i, runs, 256, utf8, sizeof(utf8));
    }

    t0 = now_s();
    for (i = 0; i < (int)iters; i++) {
        int r = edit_view_row(e, (uint16_t)(i % 26), runs, 256, utf8, sizeof(utf8));
        if (r > 0) total_runs += r;
    }
    report("view_row (142 cols)", now_s() - t0, iters, "rows");
    printf("note: %.1f runs per row on average\n",
           iters ? (double)total_runs / (double)iters : 0.0);
    printf("note: a 26-row full repaint is %.1f x the per-row cost\n", 26.0);
    free(block);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("=== edit_core host bench (regression only — NOT device numbers) ===\n");
    b_insert_char();
    b_insert_newline();
    b_gap_move();
    b_comment_cascade();
    b_view_row();
    printf("=== done ===\n");
    return 0;
}
