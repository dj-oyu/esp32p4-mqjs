/*
 * test_lines.c — 行索引。§A.1「行インデックス」と不変条件 2/3。
 *
 * ここで固定する仕様 (docs/native-editor-spec.md §A.1):
 *  - 「line_start[0] == 0、各 line_start[i] は改行の直後、line_count 個で
 *     本文を尽くす」→ 外から見えるのは edit_line_count() と
 *     edit_cursor().line1/col1 だけなので、その 2 つの整合を見る
 *  - 「カーソルの byte は文字境界、line1/col1 は byte から再導出した値と一致」
 *  - max_lines 超過 -> EDIT_E_LINES、本文は変更されていない
 *  - col1 はセル列 (CJK=2)
 *
 * 末尾改行の扱いについて: §A.1 は「line_count」の定義を書いていない。
 * ここでは **改行が n 個なら行は n+1 本** (末尾改行のあとに空行が 1 本ある)
 * という、行索引 line_start[] の定義から一意に決まる読みを固定する。
 * "a\n" は line_start = {0, 2} の 2 本。これ以外の読み (末尾の空行を
 * 数えない) だと line_start[1] が本文を尽くさなくなり不変条件 2 と衝突する。
 *
 * ---------------------------------------------------------------------------
 * このスイートが見ていないもの
 *
 *  - CR / CRLF の扱い。§A.1 は改行を LF としか書いておらず、CR を行区切りに
 *    するかどうかは未決 (報告の openQuestions を見よ)。ここでは LF だけ使う
 *  - 表示 (どの行が画面のどの段に出るか) — test_view.c
 *  - 行索引の内部表現 (gap 込みの物理オフセット) — 外から見えないので
 *    edit_check() 任せ
 *  - newline_index_moves の値。§A.1 に定義が無いので増減を見ない
 *  - タブ文字の桁数。§A.1 に「タブは何セル」の記述が無い (openQuestions)
 * ---------------------------------------------------------------------------
 */
#include "edit_test.h"

static void expect_lines(edit_t *e, uint32_t want, const char *what)
{
    uint32_t got = edit_line_count(e);
    et_checks++;
    if (got != want) {
        et_fails++;
        printf("FAIL  line_count=%u, want %u (%s)\n",
               (unsigned)got, (unsigned)want, what);
    }
}

static void t_count(void)
{
    et_ed_t ed;
    if (!et_open_default(&ed)) return;

    ET_ERR(edit_set_text(ed.e, "", 0), EDIT_OK);
    expect_lines(ed.e, 1, "empty document is one (empty) line");
    ET_OK(ed.e, "empty");

    ET_ERR(edit_set_text(ed.e, "a", 1), EDIT_OK);
    expect_lines(ed.e, 1, "no newline");

    ET_ERR(edit_set_text(ed.e, "a\nb", 3), EDIT_OK);
    expect_lines(ed.e, 2, "one newline, no trailing newline");

    /* 末尾改行: 改行 1 個なら 2 本 (2 本目は空行) */
    ET_ERR(edit_set_text(ed.e, "a\n", 2), EDIT_OK);
    expect_lines(ed.e, 2, "trailing newline leaves an empty last line");

    ET_ERR(edit_set_text(ed.e, "a\nb\n", 4), EDIT_OK);
    expect_lines(ed.e, 3, "two newlines");

    ET_ERR(edit_set_text(ed.e, "\n", 1), EDIT_OK);
    expect_lines(ed.e, 2, "a lone newline is two empty lines");

    ET_ERR(edit_set_text(ed.e, "\n\n\n", 3), EDIT_OK);
    expect_lines(ed.e, 4, "three newlines");
    ET_OK(ed.e, "line counting");

    et_close(&ed);
}

static void t_insert_delete_newlines(void)
{
    et_ed_t ed;
    if (!et_open_default(&ed)) return;

    ET_ERR(edit_set_text(ed.e, "abcd", 4), EDIT_OK);
    expect_lines(ed.e, 1, "start");

    /* 真ん中で改行 */
    ET_ERR(edit_goto(ed.e, 1, 3), EDIT_OK);
    ET_ERR(edit_insert(ed.e, "\n", 1), EDIT_OK);
    ET_IS(ed.e, "ab\ncd");
    expect_lines(ed.e, 2, "split a line in two");
    /* 挿入後のカーソルは新しい行の先頭 */
    ET_CURSOR(ed.e, 2, 1, 3);
    ET_OK(ed.e, "insert newline");

    /* もう 1 本 */
    ET_ERR(edit_insert(ed.e, "\n", 1), EDIT_OK);
    ET_IS(ed.e, "ab\n\ncd");
    expect_lines(ed.e, 3, "insert an empty line");
    ET_CURSOR(ed.e, 3, 1, 4);
    ET_OK(ed.e, "insert second newline");

    /* backspace で改行を消すと行が合流する */
    ET_ERR(edit_delete(ed.e, -1), EDIT_OK);
    ET_IS(ed.e, "ab\ncd");
    expect_lines(ed.e, 2, "backspace over a newline joins the lines");
    ET_CURSOR(ed.e, 2, 1, 3);
    ET_OK(ed.e, "delete newline backwards");

    /* 前方 delete でも同じ */
    ET_ERR(edit_move(ed.e, EDIT_M_UP, 1), EDIT_OK);
    ET_ERR(edit_move(ed.e, EDIT_M_END, 1), EDIT_OK);
    ET_CURSOR(ed.e, 1, 3, 2);
    ET_ERR(edit_delete(ed.e, 1), EDIT_OK);
    ET_IS(ed.e, "abcd");
    expect_lines(ed.e, 1, "forward delete over a newline joins the lines");
    ET_OK(ed.e, "delete newline forwards");

    /* 複数行を一気に消す (delete の n は文字数なので改行も 1 文字) */
    ET_ERR(edit_set_text(ed.e, "1\n2\n3\n4", 7), EDIT_OK);
    expect_lines(ed.e, 4, "four lines");
    ET_ERR(edit_delete(ed.e, 4), EDIT_OK);      /* "1\n2\n" を消す */
    ET_IS(ed.e, "3\n4");
    expect_lines(ed.e, 2, "removing two newlines removes two lines");
    ET_OK(ed.e, "multi-line delete");

    et_close(&ed);
}

/* §A.1 不変条件 3: line1/col1 は byte から再導出した値と一致 */
static void t_cursor_consistency(void)
{
    et_ed_t ed;
    const char *txt = "one\ntwo\n\nfour";   /* 4 行 */
    uint32_t line = 1, col = 1;
    size_t i;
    if (!et_open_default(&ed)) return;

    ET_ERR(edit_set_text(ed.e, txt, strlen(txt)), EDIT_OK);
    expect_lines(ed.e, 4, "one/two//four");

    /* 先頭から 1 文字ずつ右へ動かし、テスト側で数えた line/col と突き合わせる。
       全部 ASCII なので col の進みは 1 セル。 */
    for (i = 0; i < strlen(txt); i++) {
        edit_pos_t p = edit_cursor(ed.e);
        et_checks++;
        if (p.byte != i || p.line1 != line || p.col1 != col) {
            et_fails++;
            printf("FAIL  step %zu: cursor %u:%u@%zu, want %u:%u@%zu\n",
                   i, (unsigned)p.line1, (unsigned)p.col1, p.byte,
                   (unsigned)line, (unsigned)col, i);
        }
        if (txt[i] == '\n') { line++; col = 1; } else { col++; }
        ET_ERR(edit_move(ed.e, EDIT_M_RIGHT, 1), EDIT_OK);
    }
    ET_CURSOR(ed.e, 4, 5, strlen(txt));
    ET_OK(ed.e, "walking right across newlines");

    /* line1 は line_count を超えない */
    ET_ERR(edit_move(ed.e, EDIT_M_DOC_END, 1), EDIT_OK);
    {
        edit_pos_t p = edit_cursor(ed.e);
        ET_CHECK(p.line1 >= 1 && p.line1 <= edit_line_count(ed.e),
                 "cursor line1=%u outside 1..%u", (unsigned)p.line1,
                 (unsigned)edit_line_count(ed.e));
    }

    /* 末尾改行の直後 (空の最終行) にも立てる */
    ET_ERR(edit_set_text(ed.e, "x\n", 2), EDIT_OK);
    ET_ERR(edit_move(ed.e, EDIT_M_DOC_END, 1), EDIT_OK);
    ET_CURSOR(ed.e, 2, 1, 2);
    ET_OK(ed.e, "cursor on the empty final line");

    et_close(&ed);
}

/* §A.1「col1 はセル列 (CJK=2)」 — 行索引側から見た確認。
   詳しい端の挙動は test_cursor.c。 */
static void t_col_is_cells(void)
{
    et_ed_t ed;
    if (!et_open_default(&ed)) return;

    ET_ERR(edit_set_text(ed.e, "あa\nb", 6), EDIT_OK);
    expect_lines(ed.e, 2, "wide char then a newline");
    ET_CURSOR(ed.e, 1, 1, 0);
    ET_ERR(edit_move(ed.e, EDIT_M_RIGHT, 1), EDIT_OK);
    ET_CURSOR(ed.e, 1, 3, 3);        /* 'あ' は 2 セル */
    ET_ERR(edit_move(ed.e, EDIT_M_RIGHT, 1), EDIT_OK);
    ET_CURSOR(ed.e, 1, 4, 4);        /* 'a' は 1 セル */
    ET_ERR(edit_move(ed.e, EDIT_M_RIGHT, 1), EDIT_OK);
    ET_CURSOR(ed.e, 2, 1, 5);        /* 改行を越えて col1 は 1 に戻る */
    ET_OK(ed.e, "cell columns across a newline");

    et_close(&ed);
}

/* §A.1「max_lines 超過 -> EDIT_E_LINES。本文は変更されていない」 */
static void t_max_lines(void)
{
    et_ed_t ed;
    edit_config_t cfg = et_cfg();
    et_snap_t snap;
    char buf[64];
    int i;

    cfg.max_lines = 4;
    if (!et_open(&ed, &cfg)) return;

    /* 上限ちょうど = 改行 3 個で 4 行 */
    ET_ERR(edit_set_text(ed.e, "a\nb\nc\nd", 7), EDIT_OK);
    expect_lines(ed.e, 4, "at max_lines");
    ET_OK(ed.e, "set_text at max_lines");

    /* 5 行目は入らない。本文は 4 行のまま */
    et_snap(ed.e, &snap);
    ET_ERR(edit_set_text(ed.e, "a\nb\nc\nd\ne", 9), EDIT_E_LINES);
    ET_UNCHANGED(ed.e, &snap, "set_text over max_lines");
    expect_lines(ed.e, 4, "still four lines after a rejected set_text");
    ET_OK(ed.e, "rejected set_text");

    /* 改行の挿入も弾かれる */
    ET_ERR(edit_move(ed.e, EDIT_M_DOC_END, 1), EDIT_OK);
    ET_ERR(edit_insert(ed.e, "\n", 1), EDIT_E_LINES);
    ET_UNCHANGED(ed.e, &snap, "insert a newline at max_lines");
    expect_lines(ed.e, 4, "still four lines after a rejected insert");
    ET_OK(ed.e, "rejected newline insert");

    /* 改行を含まない挿入は通る (行数が増えないので) */
    ET_ERR(edit_insert(ed.e, "zz", 2), EDIT_OK);
    ET_IS(ed.e, "a\nb\nc\ndzz");
    expect_lines(ed.e, 4, "plain insert at max_lines is fine");
    ET_OK(ed.e, "plain insert at max_lines");

    /* 一気に 2 行増やす挿入も、増えた先が上限を超えるなら丸ごと弾かれる */
    ET_ERR(edit_set_text(ed.e, "a\nb", 3), EDIT_OK);
    et_snap(ed.e, &snap);
    ET_ERR(edit_move(ed.e, EDIT_M_DOC_END, 1), EDIT_OK);
    ET_ERR(edit_insert(ed.e, "\n\n\n", 3), EDIT_E_LINES);
    ET_UNCHANGED(ed.e, &snap, "multi-newline insert over max_lines");
    expect_lines(ed.e, 2, "still two lines");
    /* ぎりぎり収まるなら通る (2 行 + 改行 2 個 = 4 行) */
    ET_ERR(edit_insert(ed.e, "\n\n", 2), EDIT_OK);
    expect_lines(ed.e, 4, "exactly at max_lines");
    ET_OK(ed.e, "multi-newline insert boundary");

    /* 行を消してからならまた入る */
    ET_ERR(edit_delete(ed.e, -1), EDIT_OK);
    expect_lines(ed.e, 3, "after deleting a newline");
    ET_ERR(edit_insert(ed.e, "\n", 1), EDIT_OK);
    expect_lines(ed.e, 4, "room again");
    ET_OK(ed.e, "line budget recovers");

    /* 一行に長い文字列を入れても行数は増えない (max_lines と max_bytes の混同よけ) */
    ET_ERR(edit_set_text(ed.e, "", 0), EDIT_OK);
    memset(buf, 'q', sizeof(buf));
    for (i = 0; i < 8; i++) ET_ERR(edit_insert(ed.e, buf, sizeof(buf)), EDIT_OK);
    expect_lines(ed.e, 1, "512 bytes on one line");
    ET_OK(ed.e, "long single line under max_lines=4");

    et_close(&ed);
}

int main(void)
{
    printf("=== edit_core: line index ===\n");
    t_count();
    t_insert_delete_newlines();
    t_cursor_consistency();
    t_col_is_cells();
    t_max_lines();
    ET_SUMMARY("test_lines");
    return et_fails ? 1 : 0;
}
