/*
 * test_cursor.c — カーソル移動。§A.1「カーソル・選択」。
 *
 * ここで固定する仕様 (docs/native-editor-spec.md §A.1):
 *  - edit_move: 「端で clamp、OK」。端に当たってもエラーにしない
 *    (「clamp で済むものは clamp して OK を返す (呼び出し側に『端に当たった』を
 *      判断させない)」)
 *  - edit_goto: 「1 始まり (JS のエラー位置と同じ)。clamp、OK。line1==0 は E_ARG」
 *  - edit_pos_t の「col1 はセル列 (CJK=2)」
 *  - 不変条件 3「カーソルの byte は文字境界」
 *
 * ---------------------------------------------------------------------------
 * このスイートが見ていないもの
 *
 *  - UP/DOWN の「望みの桁」の記憶 (長い行 -> 短い行 -> 長い行 と動いたとき
 *    元の桁へ戻るか)。§A.1 に記述が無い。ここは「短い行では行末へ clamp」
 *    までしか見ない
 *  - PGUP/PGDN の移動量が rows か rows-1 か。§A.1 に無い。向きと clamp と
 *    「1 画面を超えない」ことだけ見る
 *  - HOME が行頭か最初の非空白か (smart home)。先頭に空白のある行では
 *    確かめない
 *  - WORD_LEFT/RIGHT が「どこに止まるか」。語の定義が §A.1 に無いので、
 *    向き・単調性・clamp と「語境界に止まる」性質だけを見る。
 *    CJK の語分割は一切見ない
 *  - move(m, 0) の意味。§A.1 に無い
 *  - 自動追従スクロールでカーソルが画面のどこに来るか — test_view.c
 *  - 選択がある状態での移動 (アンカーが残るか) — test_select.c
 * ---------------------------------------------------------------------------
 */
#include "edit_test.h"

/* テスト側が持つ文字クラス。「語境界」の判定にだけ使う。
   実装がどの規約を採っていても、クラスが変わる位置は必ず境界。 */
static int cclass(unsigned char c)
{
    if (c == ' ' || c == '\t' || c == '\n') return 0;              /* 空白 */
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '_') return 1;              /* 語 */
    return 2;                                                      /* 記号 */
}

static void expect_boundary(const char *txt, size_t len, size_t at, const char *what)
{
    et_checks++;
    if (at == 0 || at >= len) return;         /* 文書の端は常に境界 */
    if (cclass((unsigned char)txt[at - 1]) == cclass((unsigned char)txt[at])) {
        et_fails++;
        printf("FAIL  %s: word motion stopped at byte %zu, in the middle of a run "
               "of '%c'-class characters\n", what, at, txt[at]);
    }
}

/* ------------------------------------------------------------- 端の clamp */

static void t_edges(void)
{
    et_ed_t ed;
    if (!et_open_default(&ed)) return;

    ET_ERR(edit_set_text(ed.e, "ab\ncd\nef", 8), EDIT_OK);

    /* 先頭で LEFT / UP / PGUP / HOME は clamp して OK */
    ET_CURSOR(ed.e, 1, 1, 0);
    ET_ERR(edit_move(ed.e, EDIT_M_LEFT, 1), EDIT_OK);
    ET_CURSOR(ed.e, 1, 1, 0);
    ET_ERR(edit_move(ed.e, EDIT_M_LEFT, 1000), EDIT_OK);
    ET_CURSOR(ed.e, 1, 1, 0);
    ET_ERR(edit_move(ed.e, EDIT_M_HOME, 1), EDIT_OK);
    ET_CURSOR(ed.e, 1, 1, 0);
    ET_ERR(edit_move(ed.e, EDIT_M_PGUP, 1), EDIT_OK);
    ET_CHECK(edit_cursor(ed.e).line1 == 1, "PGUP at the top landed on line %u",
             (unsigned)edit_cursor(ed.e).line1);
    ET_ERR(edit_move(ed.e, EDIT_M_UP, 1), EDIT_OK);
    ET_CHECK(edit_cursor(ed.e).line1 == 1, "UP at the top landed on line %u",
             (unsigned)edit_cursor(ed.e).line1);
    ET_ERR(edit_move(ed.e, EDIT_M_DOC_HOME, 1), EDIT_OK);
    ET_CURSOR(ed.e, 1, 1, 0);
    ET_OK(ed.e, "clamped at the head");

    /* 末尾で RIGHT / DOWN / PGDN / END は clamp して OK */
    ET_ERR(edit_move(ed.e, EDIT_M_DOC_END, 1), EDIT_OK);
    ET_CURSOR(ed.e, 3, 3, 8);
    ET_ERR(edit_move(ed.e, EDIT_M_RIGHT, 1), EDIT_OK);
    ET_CURSOR(ed.e, 3, 3, 8);
    ET_ERR(edit_move(ed.e, EDIT_M_RIGHT, 1000), EDIT_OK);
    ET_CURSOR(ed.e, 3, 3, 8);
    ET_ERR(edit_move(ed.e, EDIT_M_DOWN, 1), EDIT_OK);
    ET_CHECK(edit_cursor(ed.e).line1 == 3, "DOWN at the bottom landed on line %u",
             (unsigned)edit_cursor(ed.e).line1);
    ET_ERR(edit_move(ed.e, EDIT_M_PGDN, 1), EDIT_OK);
    ET_CHECK(edit_cursor(ed.e).line1 == 3, "PGDN at the bottom landed on line %u",
             (unsigned)edit_cursor(ed.e).line1);
    ET_ERR(edit_move(ed.e, EDIT_M_END, 1), EDIT_OK);
    ET_CURSOR(ed.e, 3, 3, 8);
    ET_ERR(edit_move(ed.e, EDIT_M_DOC_END, 1), EDIT_OK);
    ET_CURSOR(ed.e, 3, 3, 8);
    ET_OK(ed.e, "clamped at the tail");

    /* 空の本文でも全 motion が clamp して OK */
    ET_ERR(edit_set_text(ed.e, "", 0), EDIT_OK);
    {
        int m;
        for (m = EDIT_M_LEFT; m <= EDIT_M_WORD_RIGHT; m++) {
            ET_ERR(edit_move(ed.e, (edit_motion_t)m, 1), EDIT_OK);
            ET_CURSOR(ed.e, 1, 1, 0);
        }
    }
    ET_OK(ed.e, "every motion on an empty document");

    et_close(&ed);
}

static void t_home_end_updown(void)
{
    et_ed_t ed;
    if (!et_open_default(&ed)) return;

    /* 先頭に空白の無い行だけを使う (smart home の有無を判定しないため) */
    ET_ERR(edit_set_text(ed.e, "long line\nab\nanother line", 25), EDIT_OK);

    ET_ERR(edit_goto(ed.e, 1, 5), EDIT_OK);
    ET_CURSOR(ed.e, 1, 5, 4);
    ET_ERR(edit_move(ed.e, EDIT_M_END, 1), EDIT_OK);
    ET_CURSOR(ed.e, 1, 10, 9);           /* "long line" は 9 文字 */
    ET_ERR(edit_move(ed.e, EDIT_M_HOME, 1), EDIT_OK);
    ET_CURSOR(ed.e, 1, 1, 0);
    ET_OK(ed.e, "home/end");

    /* DOWN で短い行へ行くと行末へ clamp する */
    ET_ERR(edit_move(ed.e, EDIT_M_END, 1), EDIT_OK);
    ET_ERR(edit_move(ed.e, EDIT_M_DOWN, 1), EDIT_OK);
    {
        edit_pos_t p = edit_cursor(ed.e);
        ET_CHECK(p.line1 == 2, "DOWN landed on line %u, want 2", (unsigned)p.line1);
        ET_CHECK(p.col1 <= 3, "col1=%u on a 2-character line (max col1 is 3)",
                 (unsigned)p.col1);
        ET_CHECK(p.byte >= 10 && p.byte <= 12,
                 "byte=%zu is outside line 2 (10..12)", p.byte);
    }
    ET_OK(ed.e, "down onto a shorter line");

    /* n 回ぶん動く */
    ET_ERR(edit_move(ed.e, EDIT_M_DOC_HOME, 1), EDIT_OK);
    ET_ERR(edit_move(ed.e, EDIT_M_RIGHT, 3), EDIT_OK);
    ET_CURSOR(ed.e, 1, 4, 3);
    ET_ERR(edit_move(ed.e, EDIT_M_DOWN, 2), EDIT_OK);
    ET_CHECK(edit_cursor(ed.e).line1 == 3, "DOWN x2 landed on line %u, want 3",
             (unsigned)edit_cursor(ed.e).line1);
    ET_OK(ed.e, "repeat count");

    et_close(&ed);
}

/* PGUP/PGDN。移動量は仕様に無いので、向き・1 画面以内・clamp だけ。 */
static void t_page(void)
{
    et_ed_t ed;
    edit_config_t cfg = et_cfg();
    char doc[256];
    size_t n = 0;
    int i;
    uint32_t rows;

    cfg.max_lines = 64;
    cfg.max_rows = 8;
    if (!et_open(&ed, &cfg)) return;
    rows = cfg.max_rows;
    edit_set_view(ed.e, cfg.max_cols, cfg.max_rows);

    for (i = 0; i < 40; i++) { doc[n++] = (char)('a' + i % 26); doc[n++] = '\n'; }
    ET_ERR(edit_set_text(ed.e, doc, n), EDIT_OK);
    ET_CHECK(edit_line_count(ed.e) == 41, "line_count=%u, want 41",
             (unsigned)edit_line_count(ed.e));

    ET_ERR(edit_move(ed.e, EDIT_M_DOC_HOME, 1), EDIT_OK);
    ET_ERR(edit_move(ed.e, EDIT_M_PGDN, 1), EDIT_OK);
    {
        uint32_t l = edit_cursor(ed.e).line1;
        ET_CHECK(l > 1, "PGDN did not move (line %u)", (unsigned)l);
        ET_CHECK(l <= 1 + rows, "PGDN moved %u lines, more than one screen (%u rows)",
                 (unsigned)(l - 1), (unsigned)rows);
    }
    ET_OK(ed.e, "page down");

    ET_ERR(edit_move(ed.e, EDIT_M_PGUP, 1), EDIT_OK);
    ET_CHECK(edit_cursor(ed.e).line1 == 1, "PGUP did not come back to line 1 (line %u)",
             (unsigned)edit_cursor(ed.e).line1);
    ET_OK(ed.e, "page up");

    /* 何度も PGDN しても文書の外へ出ない */
    for (i = 0; i < 20; i++) ET_ERR(edit_move(ed.e, EDIT_M_PGDN, 1), EDIT_OK);
    ET_CHECK(edit_cursor(ed.e).line1 == edit_line_count(ed.e),
             "after 20 PGDN the cursor is on line %u of %u",
             (unsigned)edit_cursor(ed.e).line1, (unsigned)edit_line_count(ed.e));
    ET_CHECK(edit_cursor(ed.e).byte <= edit_text_len(ed.e),
             "cursor byte %zu past the end (%zu)",
             edit_cursor(ed.e).byte, edit_text_len(ed.e));
    for (i = 0; i < 20; i++) ET_ERR(edit_move(ed.e, EDIT_M_PGUP, 1), EDIT_OK);
    ET_CHECK(edit_cursor(ed.e).line1 == 1, "after 20 PGUP the cursor is on line %u",
             (unsigned)edit_cursor(ed.e).line1);
    ET_OK(ed.e, "page motion clamps");

    et_close(&ed);
}

/* WORD_LEFT / WORD_RIGHT。語の定義は §A.1 に無いので性質だけ見る。 */
static void t_word(void)
{
    et_ed_t ed;
    const char *txt = "foo bar_1  baz.qux  end";
    size_t len;
    int i;
    if (!et_open_default(&ed)) return;

    len = strlen(txt);
    ET_ERR(edit_set_text(ed.e, txt, len), EDIT_OK);

    /* 右へ: 単調に増え、語境界に止まり、いつか末尾に着いて止まる */
    {
        size_t prev = 0;
        int steps = 0;
        for (i = 0; i < 40; i++) {
            size_t at;
            ET_ERR(edit_move(ed.e, EDIT_M_WORD_RIGHT, 1), EDIT_OK);
            at = edit_cursor(ed.e).byte;
            if (at == prev) break;                 /* 末尾で止まった */
            ET_CHECK(at > prev, "WORD_RIGHT went backwards: %zu -> %zu", prev, at);
            expect_boundary(txt, len, at, "WORD_RIGHT");
            prev = at;
            steps++;
        }
        ET_CHECK(steps >= 3, "WORD_RIGHT only made %d stops in \"%s\"", steps, txt);
        ET_CHECK(prev == len, "WORD_RIGHT settled at byte %zu, want the end (%zu)",
                 prev, len);
    }
    /* 末尾で clamp。もう一度呼んでも動かない */
    ET_ERR(edit_move(ed.e, EDIT_M_WORD_RIGHT, 1), EDIT_OK);
    ET_CHECK(edit_cursor(ed.e).byte == len, "WORD_RIGHT at the end moved to %zu",
             edit_cursor(ed.e).byte);
    ET_OK(ed.e, "word right");

    /* 左へ: 単調に減り、語境界に止まり、先頭で clamp */
    {
        size_t prev = edit_cursor(ed.e).byte;
        int steps = 0;
        for (i = 0; i < 40; i++) {
            size_t at;
            ET_ERR(edit_move(ed.e, EDIT_M_WORD_LEFT, 1), EDIT_OK);
            at = edit_cursor(ed.e).byte;
            if (at == prev) break;
            ET_CHECK(at < prev, "WORD_LEFT went forwards: %zu -> %zu", prev, at);
            expect_boundary(txt, len, at, "WORD_LEFT");
            prev = at;
            steps++;
        }
        ET_CHECK(steps >= 3, "WORD_LEFT only made %d stops", steps);
        ET_CHECK(prev == 0, "WORD_LEFT settled at byte %zu, want 0", prev);
    }
    ET_ERR(edit_move(ed.e, EDIT_M_WORD_LEFT, 1), EDIT_OK);
    ET_CURSOR(ed.e, 1, 1, 0);
    ET_OK(ed.e, "word left");

    /* 語が 1 つも無い本文でも clamp して OK */
    ET_ERR(edit_set_text(ed.e, "    ", 4), EDIT_OK);
    ET_ERR(edit_move(ed.e, EDIT_M_WORD_RIGHT, 1), EDIT_OK);
    ET_CHECK(edit_cursor(ed.e).byte <= 4, "byte=%zu past the end",
             edit_cursor(ed.e).byte);
    ET_ERR(edit_move(ed.e, EDIT_M_WORD_LEFT, 1), EDIT_OK);
    ET_CHECK(edit_cursor(ed.e).byte == 0, "WORD_LEFT on whitespace landed at %zu",
             edit_cursor(ed.e).byte);
    ET_OK(ed.e, "word motion over whitespace only");

    /* 改行を跨いでも byte は単調で、文書の外へ出ない */
    ET_ERR(edit_set_text(ed.e, "one\ntwo\nthree", 13), EDIT_OK);
    for (i = 0; i < 10; i++) {
        ET_ERR(edit_move(ed.e, EDIT_M_WORD_RIGHT, 1), EDIT_OK);
        ET_CHECK(edit_cursor(ed.e).byte <= 13, "byte=%zu past the end",
                 edit_cursor(ed.e).byte);
    }
    ET_CHECK(edit_cursor(ed.e).byte == 13, "WORD_RIGHT across newlines settled at %zu",
             edit_cursor(ed.e).byte);
    ET_OK(ed.e, "word motion across newlines");

    et_close(&ed);
}

/* §A.1「edit_goto: 1 始まり。clamp、OK。line1==0 は E_ARG」 */
static void t_goto(void)
{
    et_ed_t ed;
    edit_pos_t before;
    if (!et_open_default(&ed)) return;

    ET_ERR(edit_set_text(ed.e, "hello\nworld\n!", 13), EDIT_OK);

    ET_ERR(edit_goto(ed.e, 1, 1), EDIT_OK);
    ET_CURSOR(ed.e, 1, 1, 0);
    ET_ERR(edit_goto(ed.e, 2, 1), EDIT_OK);
    ET_CURSOR(ed.e, 2, 1, 6);
    ET_ERR(edit_goto(ed.e, 2, 3), EDIT_OK);
    ET_CURSOR(ed.e, 2, 3, 8);
    ET_ERR(edit_goto(ed.e, 3, 1), EDIT_OK);
    ET_CURSOR(ed.e, 3, 1, 12);
    ET_OK(ed.e, "goto is 1-based");

    /* 範囲外は clamp して OK */
    ET_ERR(edit_goto(ed.e, 999, 1), EDIT_OK);
    ET_CHECK(edit_cursor(ed.e).line1 == 3, "goto(999,1) landed on line %u, want 3",
             (unsigned)edit_cursor(ed.e).line1);
    ET_ERR(edit_goto(ed.e, 1, 999), EDIT_OK);
    ET_CURSOR(ed.e, 1, 6, 5);         /* "hello" の行末 */
    ET_ERR(edit_goto(ed.e, 999, 999), EDIT_OK);
    ET_CURSOR(ed.e, 3, 2, 13);
    ET_OK(ed.e, "goto clamps");

    /* line1 == 0 は E_ARG。カーソルは動かない (本文も状態も変えない) */
    ET_ERR(edit_goto(ed.e, 2, 2), EDIT_OK);
    before = edit_cursor(ed.e);
    ET_ERR(edit_goto(ed.e, 0, 1), EDIT_E_ARG);
    ET_CURSOR(ed.e, before.line1, before.col1, before.byte);
    ET_ERR(edit_goto(ed.e, 0, 0), EDIT_E_ARG);
    ET_CURSOR(ed.e, before.line1, before.col1, before.byte);
    ET_OK(ed.e, "goto line 0");

    et_close(&ed);
}

/* §A.1「col1 はセル列 (CJK=2)」 */
static void t_wide_columns(void)
{
    et_ed_t ed;
    if (!et_open_default(&ed)) return;

    /* "日本語" — 3 文字 / 9 バイト / 6 セル */
    ET_ERR(edit_set_text(ed.e, "日本語", 9), EDIT_OK);
    ET_CURSOR(ed.e, 1, 1, 0);
    ET_ERR(edit_move(ed.e, EDIT_M_RIGHT, 1), EDIT_OK);
    ET_CURSOR(ed.e, 1, 3, 3);
    ET_ERR(edit_move(ed.e, EDIT_M_RIGHT, 1), EDIT_OK);
    ET_CURSOR(ed.e, 1, 5, 6);
    ET_ERR(edit_move(ed.e, EDIT_M_RIGHT, 1), EDIT_OK);
    ET_CURSOR(ed.e, 1, 7, 9);        /* 行末 = 6 セルの次 */
    ET_ERR(edit_move(ed.e, EDIT_M_END, 1), EDIT_OK);
    ET_CURSOR(ed.e, 1, 7, 9);
    ET_ERR(edit_move(ed.e, EDIT_M_LEFT, 1), EDIT_OK);
    ET_CURSOR(ed.e, 1, 5, 6);
    ET_OK(ed.e, "wide characters advance two columns");

    /* 混在: "aあb" -> 列 1, 2, 4, 5 */
    ET_ERR(edit_set_text(ed.e, "aあb", 5), EDIT_OK);
    ET_CURSOR(ed.e, 1, 1, 0);
    ET_ERR(edit_move(ed.e, EDIT_M_RIGHT, 1), EDIT_OK);
    ET_CURSOR(ed.e, 1, 2, 1);
    ET_ERR(edit_move(ed.e, EDIT_M_RIGHT, 1), EDIT_OK);
    ET_CURSOR(ed.e, 1, 4, 4);
    ET_ERR(edit_move(ed.e, EDIT_M_RIGHT, 1), EDIT_OK);
    ET_CURSOR(ed.e, 1, 5, 5);
    ET_OK(ed.e, "mixed widths");

    /* 全角の右半分 (詰め物のセル) を狙った goto。§A.1 はここを決めていないので
       着地する列は見ない。見るのは「byte が文字境界に落ちること」(不変条件 3)。 */
    ET_ERR(edit_set_text(ed.e, "日本語", 9), EDIT_OK);
    ET_ERR(edit_goto(ed.e, 1, 2), EDIT_OK);
    {
        size_t b = edit_cursor(ed.e).byte;
        ET_CHECK(b == 0 || b == 3, "goto onto the filler cell of a wide character "
                 "landed at byte %zu (want a character boundary: 0 or 3)", b);
    }
    ET_ERR(edit_goto(ed.e, 1, 4), EDIT_OK);
    {
        size_t b = edit_cursor(ed.e).byte;
        ET_CHECK(b == 3 || b == 6, "goto onto a filler cell landed at byte %zu "
                 "(want 3 or 6)", b);
    }
    ET_OK(ed.e, "goto onto a filler cell");

    et_close(&ed);
}

int main(void)
{
    printf("=== edit_core: cursor ===\n");
    t_edges();
    t_home_end_updown();
    t_page();
    t_word();
    t_goto();
    t_wide_columns();
    ET_SUMMARY("test_cursor");
    return et_fails ? 1 : 0;
}
