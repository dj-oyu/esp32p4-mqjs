/*
 * test_view.c — 表示。§A.1「表示」節と edit_run_t の契約。
 *
 * ここで固定する仕様 (docs/native-editor-spec.md §A.1):
 *  - 「1 段ぶんを『同じ色の連続 (run)』に切って返す」
 *  - 「utf8 は呼び出し側のバッファに複製される —— core の内側を指す
 *     ポインタは出さない」
 *  - 「返り値は run 数、utf8_cap 不足なら負」
 *  - edit_run_t.ncells 「画面上の列と幅 (CJK は filler 込み、ui.cells の
 *    CONT 契約どおり)」
 *  - EDIT_RUN_SELECTED / EDIT_RUN_CURSOR (「この run の先頭セルがカーソル」) /
 *    EDIT_RUN_MARK
 *  - 「行 = 画面の段 (ソフトラップが無いので 1 対 1)」
 *  - EDIT_DIRTY_ALL「スクロール・set_view・set_text の後」
 *  - edit_dirty_rows「bit i = 画面 i 段目」、edit_dirty_clear
 *  - edit_scroll「タッチのドラッグ用。カーソルは動かさない」
 *  - 「カーソルが画面外へ出る操作は core が自動で追従スクロールする」
 *  - edit_cursor_view「画面外なら false」
 *
 * ---------------------------------------------------------------------------
 * このスイートが見ていないもの
 *
 *  - 字句の中身 (どの語が KEYWORD か、コメントの波及) — test_lex.c
 *  - preedit の run — test_preedit.c
 *  - 横スクロール。§A.1 は「横も同様」に自動追従するとしか書いておらず、
 *    画面外の左右をどう返すか (run の col が負にならない、途中で切る等) を
 *    決めていない。ここは 1 行が画面幅に収まる例だけを使う
 *  - 全角の filler が **utf8 バッファにも入るか**。§A.1 は ncells についてしか
 *    書いていない。ここでは「2 つの一貫した規約のどちらか」であることまで
 *    しか見ない (報告の openQuestions を見よ)
 *  - 空行にカーソルがあるとき run が 1 本出るかどうか。§A.1 に無い
 *  - タブの展開幅
 *  - runs_cap 不足のときの返り値。§A.1 は utf8_cap についてしか「負」と
 *    書いていない。ここは「runs_cap を超えて書かない」ことだけ見る
 *  - dirty_rows の bit が rows 以上に立つかどうかは、rows==8 の 1 例でしか
 *    見ていない
 * ---------------------------------------------------------------------------
 */
#include "edit_test.h"

#define DOC_LINES 20

/* "L01\nL02\n...\nL20" (末尾改行なし) */
static size_t make_doc(char *dst, size_t cap)
{
    size_t n = 0;
    int i;
    for (i = 1; i <= DOC_LINES; i++) {
        if (n + 5 > cap) break;
        if (i > 1) dst[n++] = '\n';
        dst[n++] = 'L';
        dst[n++] = (char)('0' + i / 10);
        dst[n++] = (char)('0' + i % 10);
    }
    return n;
}

static void expect_row_text(edit_t *e, uint16_t row, const char *want)
{
    et_row_t r;
    char got[256];
    et_row(e, row, &r);
    ET_ROW_WELLFORMED(&r);
    et_row_str(&r, got, sizeof(got));
    et_checks++;
    if (r.nruns < 0 || strcmp(got, want) != 0) {
        et_fails++;
        printf("FAIL  row %u shows \"%s\" (nruns=%d), want \"%s\"\n",
               (unsigned)row, got, r.nruns, want);
    }
}

/* ------------------------------------------------------------- 段の割り当て */

static void t_rows_are_lines(void)
{
    et_ed_t ed;
    char doc[256];
    size_t len;
    int i;
    if (!et_open_default(&ed)) return;

    len = make_doc(doc, sizeof(doc));
    edit_set_view(ed.e, 40, 8);
    ET_ERR(edit_set_text(ed.e, doc, len), EDIT_OK);
    ET_CHECK(edit_line_count(ed.e) == DOC_LINES, "line_count=%u, want %d",
             (unsigned)edit_line_count(ed.e), DOC_LINES);

    /* set_text はカーソルを 0 にするので、上端は 1 行目 */
    expect_row_text(ed.e, 0, "L01");
    expect_row_text(ed.e, 7, "L08");
    for (i = 0; i < 8; i++) {
        char want[16];
        sprintf(want, "L%02d", i + 1);
        expect_row_text(ed.e, (uint16_t)i, want);
    }
    ET_OK(ed.e, "rows map one to one onto lines");

    /* 画面の外の段は何も返さない */
    {
        et_row_t r;
        int n = et_row(ed.e, 8, &r);
        ET_CHECK(n <= 0, "edit_view_row(row=8) with rows=8 returned %d runs", n);
        n = et_row(ed.e, 63, &r);
        ET_CHECK(n <= 0, "edit_view_row(row=63) with rows=8 returned %d runs", n);
    }
    ET_OK(ed.e, "rows outside the view");

    /* 本文が画面より短いとき、余った段は空 */
    ET_ERR(edit_set_text(ed.e, "a\nb", 3), EDIT_OK);
    expect_row_text(ed.e, 0, "a");
    expect_row_text(ed.e, 1, "b");
    {
        et_row_t r;
        int n = et_row(ed.e, 2, &r);
        ET_CHECK(n <= 0, "row 2 of a 2-line document returned %d runs", n);
        n = et_row(ed.e, 7, &r);
        ET_CHECK(n <= 0, "row 7 of a 2-line document returned %d runs", n);
    }
    ET_OK(ed.e, "short document");

    et_close(&ed);
}

/* ------------------------------------------------------ スクロールと追従 */

static void t_scroll(void)
{
    et_ed_t ed;
    char doc[256];
    size_t len;
    edit_pos_t before;
    uint16_t row = 0xFFFF, col = 0xFFFF;
    if (!et_open_default(&ed)) return;

    len = make_doc(doc, sizeof(doc));
    edit_set_view(ed.e, 40, 8);
    ET_ERR(edit_set_text(ed.e, doc, len), EDIT_OK);
    edit_dirty_clear(ed.e);

    ET_CURSOR(ed.e, 1, 1, 0);
    before = edit_cursor(ed.e);
    ET_CHECK(edit_cursor_view(ed.e, &row, &col), "cursor at 1:1 is not on screen");
    ET_CHECK(row == 0 && col == 0, "cursor_view -> %u,%u, want 0,0",
             (unsigned)row, (unsigned)col);

    /* §A.1「edit_scroll: カーソルは動かさない」 */
    ET_ERR(edit_scroll(ed.e, 5), EDIT_OK);
    ET_CURSOR(ed.e, before.line1, before.col1, before.byte);
    expect_row_text(ed.e, 0, "L06");
    ET_OK(ed.e, "scroll down");

    /* §A.1「edit_cursor_view: 画面外なら false」 */
    ET_CHECK(!edit_cursor_view(ed.e, &row, &col),
             "cursor on line 1 reported on screen while the view starts at line 6");

    /* §A.1「スクロールの後は EDIT_DIRTY_ALL」 */
    ET_CHECK((edit_dirty_flags(ed.e) & EDIT_DIRTY_ALL) != 0,
             "scroll did not set EDIT_DIRTY_ALL (flags=0x%X)",
             (unsigned)edit_dirty_flags(ed.e));

    /* 戻る */
    ET_ERR(edit_scroll(ed.e, -5), EDIT_OK);
    expect_row_text(ed.e, 0, "L01");
    ET_CURSOR(ed.e, before.line1, before.col1, before.byte);
    ET_CHECK(edit_cursor_view(ed.e, &row, &col), "cursor is off screen again");
    ET_OK(ed.e, "scroll back");

    /* 端で clamp。上へ行きすぎても 1 行目より上は無い */
    ET_ERR(edit_scroll(ed.e, -1000), EDIT_OK);
    expect_row_text(ed.e, 0, "L01");
    ET_OK(ed.e, "scroll clamps at the top");

    /* 下へ行きすぎても最終行より下だけの画面にはならない */
    ET_ERR(edit_scroll(ed.e, 1000), EDIT_OK);
    {
        et_row_t r;
        char got[64];
        et_row(ed.e, 0, &r);
        et_row_str(&r, got, sizeof(got));
        ET_CHECK(r.nruns > 0, "after scrolling past the end the top row is empty "
                 "(nruns=%d) — the view should clamp so that some text is visible",
                 r.nruns);
        ET_CHECK(strcmp(got, "L01") != 0, "scroll(1000) did not move at all");
    }
    ET_OK(ed.e, "scroll clamps at the bottom");

    et_close(&ed);
}

/* §A.1「カーソルが画面外へ出る操作は core が自動で追従スクロールする」 */
static void t_autoscroll(void)
{
    et_ed_t ed;
    char doc[256];
    size_t len;
    uint16_t row = 0xFFFF, col = 0xFFFF;
    if (!et_open_default(&ed)) return;

    len = make_doc(doc, sizeof(doc));
    edit_set_view(ed.e, 40, 8);
    ET_ERR(edit_set_text(ed.e, doc, len), EDIT_OK);

    /* 下端の外へ */
    ET_ERR(edit_move(ed.e, EDIT_M_DOC_END, 1), EDIT_OK);
    ET_CHECK(edit_cursor_view(ed.e, &row, &col),
             "DOC_END did not scroll the cursor into view");
    ET_CHECK(row < 8, "cursor_view row=%u outside the 8-row view", (unsigned)row);
    ET_OK(ed.e, "autoscroll to the end");

    /* 上端の外へ */
    ET_ERR(edit_move(ed.e, EDIT_M_DOC_HOME, 1), EDIT_OK);
    ET_CHECK(edit_cursor_view(ed.e, &row, &col),
             "DOC_HOME did not scroll the cursor into view");
    ET_CHECK(row < 8, "cursor_view row=%u outside the view", (unsigned)row);
    expect_row_text(ed.e, 0, "L01");
    ET_OK(ed.e, "autoscroll to the start");

    /* goto でも追従する */
    ET_ERR(edit_goto(ed.e, 15, 1), EDIT_OK);
    ET_CHECK(edit_cursor_view(ed.e, &row, &col),
             "goto(15,1) did not scroll the cursor into view");
    ET_OK(ed.e, "autoscroll on goto");

    /* 手でスクロールして画面外へ出したあと、編集すると戻ってくる */
    ET_ERR(edit_goto(ed.e, 1, 1), EDIT_OK);
    ET_ERR(edit_scroll(ed.e, 10), EDIT_OK);
    ET_CHECK(!edit_cursor_view(ed.e, &row, &col),
             "cursor should be off screen after scrolling away from it");
    ET_ERR(edit_insert(ed.e, "x", 1), EDIT_OK);
    ET_CHECK(edit_cursor_view(ed.e, &row, &col),
             "an edit did not scroll the cursor back into view");
    ET_OK(ed.e, "editing scrolls back to the cursor");

    /* 1 行ずつ下へ。常に画面の中にいる */
    {
        int i;
        ET_ERR(edit_move(ed.e, EDIT_M_DOC_HOME, 1), EDIT_OK);
        for (i = 0; i < DOC_LINES + 2; i++) {
            ET_ERR(edit_move(ed.e, EDIT_M_DOWN, 1), EDIT_OK);
            ET_CHECK(edit_cursor_view(ed.e, &row, &col),
                     "cursor left the view after %d DOWN presses", i + 1);
            ET_CHECK(row < 8, "cursor_view row=%u outside the view", (unsigned)row);
        }
    }
    ET_OK(ed.e, "cursor stays visible while walking down");

    et_close(&ed);
}

/* ------------------------------------------------------------------ dirty */

static void t_dirty(void)
{
    et_ed_t ed;
    char doc[256];
    size_t len;
    uint64_t rows;
    if (!et_open_default(&ed)) return;

    len = make_doc(doc, sizeof(doc));
    ET_ERR(edit_set_text(ed.e, doc, len), EDIT_OK);

    /* §A.1「set_view: 全行 dirty」。同じ寸法で呼び直したときに no-op へ
       落とす実装もあり得るので、寸法が実際に変わる呼びで見る (回転と
       キーボード表示 = 寸法が変わる場合が §A.1 の想定)。 */
    edit_set_view(ed.e, 30, 6);
    edit_dirty_clear(ed.e);
    ET_CHECK(edit_dirty_rows(ed.e) == 0, "dirty_rows=0x%llX right after dirty_clear",
             (unsigned long long)edit_dirty_rows(ed.e));
    ET_CHECK(edit_dirty_flags(ed.e) == 0, "dirty_flags=0x%X right after dirty_clear",
             (unsigned)edit_dirty_flags(ed.e));

    edit_set_view(ed.e, 40, 8);
    ET_CHECK((edit_dirty_flags(ed.e) & EDIT_DIRTY_ALL) != 0,
             "set_view did not set EDIT_DIRTY_ALL (flags=0x%X)",
             (unsigned)edit_dirty_flags(ed.e));
    rows = edit_dirty_rows(ed.e);
    ET_CHECK(rows == 0xFFull, "set_view(40,8) -> dirty_rows=0x%llX, want 0xFF "
             "(bit i = screen row i, and there are only 8 rows)",
             (unsigned long long)rows);
    ET_OK(ed.e, "set_view dirties every row");

    /* §A.1「set_text の後」も EDIT_DIRTY_ALL */
    edit_dirty_clear(ed.e);
    ET_ERR(edit_set_text(ed.e, doc, len), EDIT_OK);
    ET_CHECK((edit_dirty_flags(ed.e) & EDIT_DIRTY_ALL) != 0,
             "set_text did not set EDIT_DIRTY_ALL (flags=0x%X)",
             (unsigned)edit_dirty_flags(ed.e));

    /* 1 行だけの編集は、その段だけを dirty にする */
    ET_ERR(edit_goto(ed.e, 3, 1), EDIT_OK);
    edit_dirty_clear(ed.e);
    ET_ERR(edit_insert(ed.e, "x", 1), EDIT_OK);
    rows = edit_dirty_rows(ed.e);
    ET_CHECK((rows & (1ull << 2)) != 0,
             "editing line 3 (screen row 2) left dirty_rows=0x%llX — bit 2 is clear",
             (unsigned long long)rows);
    ET_CHECK((edit_dirty_flags(ed.e) & EDIT_DIRTY_ALL) == 0,
             "a single-line insert set EDIT_DIRTY_ALL; §A.1 reserves that for "
             "scroll / set_view / set_text");
    ET_OK(ed.e, "single-line dirty");

    /* カーソルが別の行へ移ると旧行と新行の両方が dirty (§A.1
       「カーソル行が変わった (旧行も rows に入っている)」) */
    edit_dirty_clear(ed.e);
    ET_ERR(edit_move(ed.e, EDIT_M_DOWN, 1), EDIT_OK);
    rows = edit_dirty_rows(ed.e);
    ET_CHECK((edit_dirty_flags(ed.e) & EDIT_DIRTY_CURSOR) != 0,
             "moving the cursor to another line did not set EDIT_DIRTY_CURSOR "
             "(flags=0x%X)", (unsigned)edit_dirty_flags(ed.e));
    ET_CHECK((rows & (1ull << 2)) != 0, "the old cursor row (2) is not dirty "
             "(rows=0x%llX)", (unsigned long long)rows);
    ET_CHECK((rows & (1ull << 3)) != 0, "the new cursor row (3) is not dirty "
             "(rows=0x%llX)", (unsigned long long)rows);
    ET_OK(ed.e, "cursor move dirties both rows");

    /* dirty_clear は両方を落とす */
    edit_dirty_clear(ed.e);
    ET_CHECK(edit_dirty_rows(ed.e) == 0, "dirty_rows=0x%llX after dirty_clear",
             (unsigned long long)edit_dirty_rows(ed.e));
    ET_CHECK(edit_dirty_flags(ed.e) == 0, "dirty_flags=0x%X after dirty_clear",
             (unsigned)edit_dirty_flags(ed.e));
    ET_OK(ed.e, "dirty_clear");

    /* 段の番号が画面に一致していること。5 段目 (screen row 4) を編集する */
    ET_ERR(edit_goto(ed.e, 5, 1), EDIT_OK);
    edit_dirty_clear(ed.e);
    ET_ERR(edit_insert(ed.e, "y", 1), EDIT_OK);
    rows = edit_dirty_rows(ed.e);
    ET_CHECK((rows & (1ull << 4)) != 0, "editing screen row 4 left dirty_rows=0x%llX",
             (unsigned long long)rows);
    expect_row_text(ed.e, 4, "yL05");
    ET_OK(ed.e, "dirty bit position matches the screen row");

    et_close(&ed);
}

/* --------------------------------------------------------------- run の旗 */

static void t_flags(void)
{
    et_ed_t ed;
    et_row_t r;
    uint16_t crow = 0xFFFF, ccol = 0xFFFF;
    int i;
    if (!et_open_default(&ed)) return;

    edit_set_view(ed.e, 40, 8);
    ET_ERR(edit_set_text(ed.e, "abcdefgh\nijkl", 13), EDIT_OK);

    /* EDIT_RUN_CURSOR: 「この run の先頭セルがカーソル」 */
    ET_ERR(edit_goto(ed.e, 1, 4), EDIT_OK);
    ET_CHECK(edit_cursor_view(ed.e, &crow, &ccol), "cursor is off screen");
    ET_CHECK(crow == 0 && ccol == 3, "cursor_view -> %u,%u, want 0,3",
             (unsigned)crow, (unsigned)ccol);
    et_row(ed.e, 0, &r);
    ET_ROW_WELLFORMED(&r);
    {
        int found = -1;
        for (i = 0; i < r.nruns; i++) {
            if (r.runs[i].flags & EDIT_RUN_CURSOR) {
                ET_CHECK(found < 0, "more than one run on row 0 carries "
                         "EDIT_RUN_CURSOR");
                found = i;
            }
        }
        ET_CHECK(found >= 0, "no run on the cursor row carries EDIT_RUN_CURSOR");
        if (found >= 0) {
            ET_CHECK(r.runs[found].col == ccol,
                     "EDIT_RUN_CURSOR run starts at col %u, cursor is at col %u",
                     (unsigned)r.runs[found].col, (unsigned)ccol);
        }
    }
    /* カーソルのいない段には立たない */
    et_row(ed.e, 1, &r);
    ET_CHECK((et_row_flags(&r) & EDIT_RUN_CURSOR) == 0,
             "EDIT_RUN_CURSOR appears on a row without the cursor");
    ET_OK(ed.e, "EDIT_RUN_CURSOR");

    /* EDIT_RUN_SELECTED: 選んだセルだけに立つ */
    ET_ERR(edit_goto(ed.e, 1, 3), EDIT_OK);        /* byte 2 */
    edit_select_begin(ed.e);
    ET_ERR(edit_goto(ed.e, 1, 6), EDIT_OK);        /* byte 5 */
    et_row(ed.e, 0, &r);
    ET_ROW_WELLFORMED(&r);
    for (i = 0; i < 8; i++) {
        int idx = et_run_at(&r, (uint16_t)i);
        bool want = (i >= 2 && i < 5);
        bool got;
        et_checks++;
        if (idx < 0) {
            et_fails++;
            printf("FAIL  no run covers col %d of \"abcdefgh\"\n", i);
            continue;
        }
        got = (r.runs[idx].flags & EDIT_RUN_SELECTED) != 0;
        if (got != want) {
            et_fails++;
            printf("FAIL  col %d: EDIT_RUN_SELECTED=%d, want %d (selection is "
                   "bytes 2..5)\n", i, (int)got, (int)want);
        }
    }
    edit_select_end(ed.e);
    et_row(ed.e, 0, &r);
    ET_CHECK((et_row_flags(&r) & EDIT_RUN_SELECTED) == 0,
             "EDIT_RUN_SELECTED survived select_end");
    ET_OK(ed.e, "EDIT_RUN_SELECTED");

    /* EDIT_RUN_MARK: set_mark した行に立ち、clear_mark で消える。
       §A.1 は「行に立つ」か「その桁だけ」かを決めていないので、
       行の有無だけを見る。 */
    ET_ERR(edit_set_mark(ed.e, 2, 2), EDIT_OK);
    et_row(ed.e, 1, &r);
    ET_ROW_WELLFORMED(&r);
    ET_CHECK((et_row_flags(&r) & EDIT_RUN_MARK) != 0,
             "set_mark(2,2) did not put EDIT_RUN_MARK on screen row 1");
    et_row(ed.e, 0, &r);
    ET_CHECK((et_row_flags(&r) & EDIT_RUN_MARK) == 0,
             "EDIT_RUN_MARK leaked onto row 0");
    ET_OK(ed.e, "EDIT_RUN_MARK");

    edit_clear_mark(ed.e);
    et_row(ed.e, 1, &r);
    ET_CHECK((et_row_flags(&r) & EDIT_RUN_MARK) == 0,
             "EDIT_RUN_MARK survived clear_mark");
    ET_OK(ed.e, "clear_mark");

    et_close(&ed);
}

/* ---------------------------------------------------------- 全角と ncells */

static void t_wide_cells(void)
{
    et_ed_t ed;
    et_row_t r;
    unsigned cells, cps;
    if (!et_open_default(&ed)) return;

    edit_set_view(ed.e, 40, 8);

    /* ASCII だけ: ncells = 文字数 */
    ET_ERR(edit_set_text(ed.e, "abcd", 4), EDIT_OK);
    et_row(ed.e, 0, &r);
    ET_ROW_WELLFORMED(&r);
    ET_CHECK(et_row_cells(&r) == 4, "ncells total = %u for \"abcd\", want 4",
             et_row_cells(&r));

    /* CJK 3 文字 = 6 セル (§A.1「CJK は filler 込み」) */
    ET_ERR(edit_set_text(ed.e, "日本語", 9), EDIT_OK);
    et_row(ed.e, 0, &r);
    ET_ROW_WELLFORMED(&r);
    cells = et_row_cells(&r);
    ET_CHECK(cells == 6, "ncells total = %u for 3 CJK characters, want 6", cells);
    ET_OK(ed.e, "CJK ncells");

    /* utf8 に filler の詰め物コードポイントが入るかどうかは §A.1 が決めて
       いない。ここでは「一貫した 2 つの規約のどちらか」であることだけを
       確かめ、どちらだったかを表示する (報告の openQuestions を見よ)。 */
    {
        char row_txt[256];
        size_t nb = et_row_str(&r, row_txt, sizeof(row_txt));
        cps = et_cp_count(row_txt, nb);
        printf("note: 3 CJK chars -> ncells=%u, utf8 codepoints=%u (%s)\n",
               cells, cps,
               (cells == 6 && cps == 6) ? "filler codepoints ARE in the utf8 buffer"
               : (cells == 6 && cps == 3) ? "filler codepoints are NOT in the utf8 buffer"
               : "neither — see the failure above");
        ET_CHECK(cells == 6 && (cps == cells || cps == 3),
                 "row utf8 has %u codepoints for 3 CJK characters occupying %u "
                 "cells — neither convention (with fillers = %u, without = 3)",
                 cps, cells, cells);
    }

    /* 混在: "aあb" = 1 + 2 + 1 = 4 セル */
    ET_ERR(edit_set_text(ed.e, "aあb", 5), EDIT_OK);
    et_row(ed.e, 0, &r);
    ET_ROW_WELLFORMED(&r);
    ET_CHECK(et_row_cells(&r) == 4, "ncells total = %u for \"a<wide>b\", want 4",
             et_row_cells(&r));
    ET_OK(ed.e, "mixed widths");

    /* 全角にカーソルを置いたとき、run の col はセル列 */
    ET_ERR(edit_set_text(ed.e, "あいう", 9), EDIT_OK);
    ET_ERR(edit_move(ed.e, EDIT_M_RIGHT, 1), EDIT_OK);
    {
        uint16_t crow = 0xFFFF, ccol = 0xFFFF;
        ET_CHECK(edit_cursor_view(ed.e, &crow, &ccol), "cursor is off screen");
        ET_CHECK(ccol == 2, "cursor_view col=%u after one RIGHT over a wide "
                 "character, want 2", (unsigned)ccol);
    }
    ET_OK(ed.e, "cursor column over wide characters");

    et_close(&ed);
}

/* ------------------------------------------------- バッファ不足と溢れ防止 */

static void t_caps(void)
{
    et_ed_t ed;
    edit_run_t runs[64];
    char utf8[256];
    int n;
    int i;
    if (!et_open_default(&ed)) return;

    edit_set_view(ed.e, 40, 8);
    /* 色が交互に変わる = run が多い行 */
    ET_ERR(edit_set_text(ed.e, "a+b+c+d+e+f+g+h+i+j+", 20), EDIT_OK);

    /* §A.1「utf8_cap 不足なら負」 */
    memset(utf8, 0x5A, sizeof(utf8));
    n = edit_view_row(ed.e, 0, runs, 64, utf8, 1);
    ET_CHECK(n < 0, "edit_view_row with utf8_cap=1 on a 20-byte row returned %d, "
             "want a negative value", n);
    ET_CHECK(utf8[1] == 0x5A, "edit_view_row wrote past utf8_cap=1");
    /* cap=1 の呼び出しは utf8[0] を正当に埋めるので、cap=0 のカナリアを
       張り直してから見る (張り直さないと自分が壊した跡を実装のせいにする) */
    memset(utf8, 0x5A, sizeof(utf8));
    n = edit_view_row(ed.e, 0, runs, 64, utf8, 0);
    ET_CHECK(n < 0, "edit_view_row with utf8_cap=0 returned %d, want negative", n);
    ET_CHECK(utf8[0] == 0x5A, "edit_view_row wrote into a zero-length buffer");
    ET_OK(ed.e, "utf8_cap too small");

    /* ちょうど足りるなら通る */
    n = edit_view_row(ed.e, 0, runs, 64, utf8, sizeof(utf8));
    ET_CHECK(n > 0, "edit_view_row with a big enough buffer returned %d", n);

    /* runs_cap 不足。返り値は §A.1 に無いので、書きすぎないことだけ見る。 */
    for (i = 0; i < 64; i++) memset(&runs[i], 0x5A, sizeof(runs[i]));
    n = edit_view_row(ed.e, 0, runs, 2, utf8, sizeof(utf8));
    ET_CHECK(n <= 2, "edit_view_row with runs_cap=2 returned %d runs", n);
    {
        edit_run_t canary;
        memset(&canary, 0x5A, sizeof(canary));
        ET_CHECK(memcmp(&runs[2], &canary, sizeof(canary)) == 0,
                 "edit_view_row wrote past runs_cap=2");
        ET_CHECK(memcmp(&runs[63], &canary, sizeof(canary)) == 0,
                 "edit_view_row wrote far past runs_cap=2");
    }
    n = edit_view_row(ed.e, 0, runs, 0, utf8, sizeof(utf8));
    ET_CHECK(n <= 0, "edit_view_row with runs_cap=0 returned %d", n);
    ET_OK(ed.e, "runs_cap too small");

    /* 失敗しても内部状態を壊していない: もう一度普通に引ける */
    n = edit_view_row(ed.e, 0, runs, 64, utf8, sizeof(utf8));
    ET_CHECK(n > 0, "edit_view_row returned %d after the capped calls", n);
    ET_OK(ed.e, "state survives capped calls");

    et_close(&ed);
}

int main(void)
{
    printf("=== edit_core: view ===\n");
    t_rows_are_lines();
    t_scroll();
    t_autoscroll();
    t_dirty();
    t_flags();
    t_wide_cells();
    t_caps();
    ET_SUMMARY("test_view");
    return et_fails ? 1 : 0;
}
