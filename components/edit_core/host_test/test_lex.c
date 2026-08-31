/*
 * test_lex.c — 行単位の字句解析。§A.1「字句解析」と line_state。
 *
 * ここで固定する仕様 (docs/native-editor-spec.md §A.1):
 *  - 「字句解析は行単位、行頭状態から。編集行を再字句し、行末状態が前回と
 *     違う間だけ次行へ波及」
 *  - 「波及は**可視域の下端で打ち切り**、残りは次に edit_view_row が
 *     呼ばれたとき遅延で解く (要再計算ビット)」
 *  - line_state の 3 値「0 素, 1 ブロックコメント内, 2 テンプレート文字列内」
 *    -> ブロックコメントとテンプレート文字列だけが行を跨ぐ
 *  - 「**正規表現リテラルは検出しない (`/` は PUNCT)**。理由: `/` の文脈判定は
 *     行内で閉じないので行単位字句の前提を壊す。誤彩色は害が無い」
 *  - 不変条件 4「line_state[i] は line_state[i-1] から行 i-1 を字句解析した
 *    結果と一致 (要再計算ビットの立った行は除く)」-> edit_check() で見る
 *
 * ---------------------------------------------------------------------------
 * このスイートが見ていないもの
 *
 *  - キーワードの一覧。§A.1 は EDIT_CLS_KEYWORD という枠を決めただけで、
 *    どの語が入るかを書いていない。ここでは JS の予約語のうち疑いようの
 *    ないものを数語だけ見る (t_keywords)。一覧そのものは openQuestions
 *  - EDIT_CLS_IDENT と EDIT_CLS_PLAIN の使い分け。§A.1 に無いので、
 *    識別子がどちらになるかは見ない
 *  - 空白のクラス。§A.1 に無い
 *  - 数値リテラルの文法 (16 進、指数、区切りの _)。素の 10 進だけ見る
 *  - 文字列のエスケープ (\" や \\ の直後で閉じないこと) は 1 例だけ
 *  - テンプレート文字列の ${} の中を式として色分けするか。§A.1 に無い
 *  - relex_cascades の値。§A.1 に定義が無いので relex_lines の増分しか見ない
 *  - 波及の打ち切り位置が「可視域の下端ちょうど」か。ここでは
 *    「文書全体を舐めてはいない」ことしか見ない
 * ---------------------------------------------------------------------------
 */
#include "edit_test.h"

#define LEX_ROWS  8

/* line1 行目の col セル目の字句クラス。行が画面外なら goto が追従スクロール
   するので、そこで初めて遅延の再字句が起きる。 */
static unsigned cls_at(edit_t *e, uint32_t line1, uint16_t col)
{
    uint16_t row = 0xFFFF, c = 0xFFFF;
    et_row_t r;

    if (edit_goto(e, line1, 1) != EDIT_OK) return (unsigned)EDIT_CLS_N;
    if (!edit_cursor_view(e, &row, &c)) return (unsigned)EDIT_CLS_N;
    et_row(e, row, &r);
    ET_ROW_WELLFORMED(&r);
    return et_cls_at(&r, col);
}

static void expect_cls(edit_t *e, uint32_t line1, uint16_t col, unsigned want,
                       const char *what)
{
    unsigned got = cls_at(e, line1, col);
    et_checks++;
    if (got != want) {
        et_fails++;
        printf("FAIL  %s: line %u col %u is %s, want %s\n", what,
               (unsigned)line1, (unsigned)col, et_cls_name(got), et_cls_name(want));
    }
}

static void expect_not_cls(edit_t *e, uint32_t line1, uint16_t col, unsigned not_want,
                           const char *what)
{
    unsigned got = cls_at(e, line1, col);
    et_checks++;
    if (got == not_want) {
        et_fails++;
        printf("FAIL  %s: line %u col %u is %s, and must not be\n", what,
               (unsigned)line1, (unsigned)col, et_cls_name(got));
    }
}

static uint32_t relex_lines(const edit_t *e)
{
    edit_stats_t st;
    memset(&st, 0, sizeof(st));
    edit_stats(e, &st);
    return st.relex_lines;
}

/* -------------------------------------------------------- 1 行で閉じるもの */

static void t_single_line(void)
{
    et_ed_t ed;
    if (!et_open_default(&ed)) return;
    edit_set_view(ed.e, 40, LEX_ROWS);

    /* 行コメント */
    ET_ERR(edit_set_text(ed.e, "a = 1; // note\nb = 2;", 21), EDIT_OK);
    expect_cls(ed.e, 1, 7, EDIT_CLS_COMMENT, "line comment start");
    expect_cls(ed.e, 1, 8, EDIT_CLS_COMMENT, "line comment second slash");
    expect_cls(ed.e, 1, 10, EDIT_CLS_COMMENT, "line comment body");
    expect_not_cls(ed.e, 2, 0, EDIT_CLS_COMMENT, "a line comment does not cascade");
    ET_OK(ed.e, "line comment");

    /* 文字列 */
    ET_ERR(edit_set_text(ed.e, "s = \"abc\";\nt = 'x';\nu = 0;", 26), EDIT_OK);
    expect_cls(ed.e, 1, 5, EDIT_CLS_STRING, "double-quoted string body");
    expect_not_cls(ed.e, 1, 9, EDIT_CLS_STRING, "after the closing quote");
    expect_cls(ed.e, 2, 5, EDIT_CLS_STRING, "single-quoted string body");
    expect_not_cls(ed.e, 3, 4, EDIT_CLS_STRING, "a quoted string does not cascade");
    ET_OK(ed.e, "strings");

    /* エスケープされた引用符では閉じない */
    ET_ERR(edit_set_text(ed.e, "s = \"a\\\"b\";\nt = 0;", 18), EDIT_OK);
    expect_cls(ed.e, 1, 8, EDIT_CLS_STRING, "after an escaped quote");
    expect_not_cls(ed.e, 2, 4, EDIT_CLS_STRING, "escaped quote did not leak a line");
    ET_OK(ed.e, "escaped quote");

    /* 数値 */
    ET_ERR(edit_set_text(ed.e, "n = 1234;", 9), EDIT_OK);
    expect_cls(ed.e, 1, 4, EDIT_CLS_NUMBER, "decimal literal");
    expect_cls(ed.e, 1, 7, EDIT_CLS_NUMBER, "decimal literal end");
    ET_OK(ed.e, "number");

    /* 同じ行で閉じるブロックコメント */
    ET_ERR(edit_set_text(ed.e, "a /* c */ b;\nc = 1;", 19), EDIT_OK);
    expect_cls(ed.e, 1, 3, EDIT_CLS_COMMENT, "inline block comment");
    expect_not_cls(ed.e, 1, 10, EDIT_CLS_COMMENT, "after the inline block comment");
    expect_not_cls(ed.e, 2, 0, EDIT_CLS_COMMENT,
                   "a block comment closed on the same line does not cascade");
    ET_OK(ed.e, "inline block comment");

    et_close(&ed);
}

/*
 * §A.1「正規表現リテラルは検出しない (`/` は PUNCT)」。
 * これは**決定**であって手抜きではないので、テストで固定する。
 * 将来「正規表現を色分けしよう」と誰かが実装すると、ここが赤くなって
 * 台帳 (§E #12) を読み直すことになる。
 */
static void t_slash_is_punct(void)
{
    et_ed_t ed;
    if (!et_open_default(&ed)) return;
    edit_set_view(ed.e, 40, LEX_ROWS);

    /* 割り算 */
    ET_ERR(edit_set_text(ed.e, "x = a / b;", 10), EDIT_OK);
    expect_cls(ed.e, 1, 6, EDIT_CLS_PUNCT, "division slash");
    ET_OK(ed.e, "division");

    /* 正規表現に見えるもの。'/' は PUNCT のまま、中身は文字列にしない */
    ET_ERR(edit_set_text(ed.e, "re = /abc/g;\nnext = 1;", 22), EDIT_OK);
    expect_cls(ed.e, 1, 5, EDIT_CLS_PUNCT, "regexp-looking slash");
    expect_not_cls(ed.e, 1, 6, EDIT_CLS_STRING, "regexp body is not a string");
    expect_not_cls(ed.e, 1, 6, EDIT_CLS_COMMENT, "regexp body is not a comment");
    expect_cls(ed.e, 1, 9, EDIT_CLS_PUNCT, "regexp closing slash");
    /* 決定的に大事なところ: 未閉じに見えても次の行へ波及しない */
    expect_not_cls(ed.e, 2, 0, EDIT_CLS_STRING, "no cascade from a slash");
    expect_not_cls(ed.e, 2, 0, EDIT_CLS_COMMENT, "no cascade from a slash");
    ET_OK(ed.e, "slash never cascades");

    /* 奇数個の '/' が並んでも状態が残らない */
    ET_ERR(edit_set_text(ed.e, "a = 1 / 2 / 3 /\nb = 4;", 22), EDIT_OK);
    expect_not_cls(ed.e, 2, 0, EDIT_CLS_STRING, "trailing slash does not cascade");
    expect_not_cls(ed.e, 2, 0, EDIT_CLS_COMMENT, "trailing slash does not cascade");
    ET_OK(ed.e, "trailing slash");

    et_close(&ed);
}

/* ------------------------------------------- 行を跨ぐ 2 つの状態 */

static void t_template_string(void)
{
    et_ed_t ed;
    if (!et_open_default(&ed)) return;
    edit_set_view(ed.e, 40, LEX_ROWS);

    /* "var s = `abc\ndef`;\nvar t = 1;" */
    ET_ERR(edit_set_text(ed.e, "var s = `abc\ndef`;\nvar t = 1;", 29), EDIT_OK);
    expect_cls(ed.e, 1, 9, EDIT_CLS_STRING, "template body on the opening line");
    expect_cls(ed.e, 2, 0, EDIT_CLS_STRING, "template body carried to the next line");
    expect_cls(ed.e, 2, 2, EDIT_CLS_STRING, "template body before the close");
    expect_not_cls(ed.e, 2, 4, EDIT_CLS_STRING, "after the closing backtick");
    expect_not_cls(ed.e, 3, 0, EDIT_CLS_STRING, "the template ended on line 2");
    ET_OK(ed.e, "template string across lines");

    /* 閉じずに終わる: 最終行まで文字列のまま。壊れないこと */
    ET_ERR(edit_set_text(ed.e, "var s = `abc\ndef\nghi", 20), EDIT_OK);
    expect_cls(ed.e, 2, 0, EDIT_CLS_STRING, "unterminated template, line 2");
    expect_cls(ed.e, 3, 0, EDIT_CLS_STRING, "unterminated template, line 3");
    ET_OK(ed.e, "unterminated template");

    /* 閉じを足すと下の行が解ける */
    ET_ERR(edit_goto(ed.e, 2, 4), EDIT_OK);
    ET_ERR(edit_insert(ed.e, "`", 1), EDIT_OK);
    expect_not_cls(ed.e, 3, 0, EDIT_CLS_STRING,
                   "closing the template released the line below");
    ET_OK(ed.e, "closing a template");

    et_close(&ed);
}

/*
 * ブロックコメントの波及。§A.1 の中心。
 *
 *   line 1      var a = 1;
 *   line 2..20  foo();
 *   line 21     bar(); *<slash>
 *   line 22     var b = 2;
 *
 * 何もしていない状態では line 21 の閉じ記号は宙に浮いた記号でしかない。
 * line 2 の頭に開始記号を挿すと、line 2..21 がコメントになり line 22 で
 * 止まる。画面は 8 段しかないので、波及は line 8 あたりで打ち切られ、
 * line 9 以降は下へスクロールしたときに遅延で解ける。
 */
static void t_block_comment_cascade(void)
{
    et_ed_t ed;
    char doc[512];
    size_t n = 0;
    int i;
    uint32_t r0, r1;

    if (!et_open_default(&ed)) return;
    edit_set_view(ed.e, 40, LEX_ROWS);

    n += (size_t)sprintf(doc + n, "var a = 1;\n");
    for (i = 2; i <= 20; i++) n += (size_t)sprintf(doc + n, "foo();\n");
    n += (size_t)sprintf(doc + n, "bar(); *%c\n", '/');
    n += (size_t)sprintf(doc + n, "var b = 2;");
    ET_ERR(edit_set_text(ed.e, doc, n), EDIT_OK);
    ET_CHECK(edit_line_count(ed.e) == 22, "line_count=%u, want 22",
             (unsigned)edit_line_count(ed.e));

    /* まだコメントは開いていない */
    expect_not_cls(ed.e, 5, 0, EDIT_CLS_COMMENT, "before opening a block comment");
    expect_not_cls(ed.e, 22, 0, EDIT_CLS_COMMENT, "before opening a block comment");
    ET_OK(ed.e, "no comment yet");

    /* 画面を上端へ戻してから開始記号を挿す */
    ET_ERR(edit_goto(ed.e, 1, 1), EDIT_OK);
    ET_ERR(edit_goto(ed.e, 2, 1), EDIT_OK);
    r0 = relex_lines(ed.e);
    ET_ERR(edit_insert(ed.e, "/*", 2), EDIT_OK);
    r1 = relex_lines(ed.e);

    /*
     * §A.1「波及は可視域の下端で打ち切り」。画面は 8 段なので、
     * 1 打鍵で 22 行ぜんぶを舐めてはいけない。
     */
    printf("note: relex_lines +%u for one '/*' with an %d-row view (22 lines)\n",
           (unsigned)(r1 - r0), LEX_ROWS);
    ET_CHECK(r1 > r0, "relex_lines did not move (+%u) — the edited line must be "
             "re-lexed", (unsigned)(r1 - r0));
    ET_CHECK(r1 - r0 <= LEX_ROWS + 4,
             "relex_lines +%u for one keystroke with a %d-row view — the cascade "
             "was not truncated at the bottom of the visible area (§A.1)",
             (unsigned)(r1 - r0), LEX_ROWS);
    ET_OK(ed.e, "cascade truncated");

    /* 可視域の中はもう正しい */
    expect_cls(ed.e, 2, 0, EDIT_CLS_COMMENT, "the line that opens the comment");
    expect_cls(ed.e, 3, 0, EDIT_CLS_COMMENT, "the line below, inside the view");
    expect_cls(ed.e, 8, 0, EDIT_CLS_COMMENT, "the bottom of the view");
    ET_OK(ed.e, "visible cascade");

    /*
     * 打ち切った下も、view_row を通せば正しくなる (遅延)。
     * 「打ち切った直後でも最終的な彩色は正しい」がここ。
     */
    expect_cls(ed.e, 9, 0, EDIT_CLS_COMMENT, "just past the truncation point");
    expect_cls(ed.e, 15, 0, EDIT_CLS_COMMENT, "deep below the truncation point");
    expect_cls(ed.e, 20, 0, EDIT_CLS_COMMENT, "the last line inside the comment");
    expect_cls(ed.e, 21, 0, EDIT_CLS_COMMENT, "the line carrying the close");
    /* 閉じたところで止まる */
    expect_not_cls(ed.e, 22, 0, EDIT_CLS_COMMENT,
                   "the comment must stop at the closing marker on line 21");
    ET_OK(ed.e, "lazy resolution below the truncation point");

    /* 下から上へ読み直しても同じ (遅延の解き方が読む順に依存しない) */
    expect_not_cls(ed.e, 22, 0, EDIT_CLS_COMMENT, "re-read line 22");
    expect_cls(ed.e, 12, 0, EDIT_CLS_COMMENT, "re-read line 12");
    expect_cls(ed.e, 3, 0, EDIT_CLS_COMMENT, "re-read line 3");
    ET_OK(ed.e, "order independent");

    /* 取り消すと全部戻る */
    ET_ERR(edit_undo(ed.e), EDIT_OK);
    expect_not_cls(ed.e, 3, 0, EDIT_CLS_COMMENT, "after undo, near the top");
    expect_not_cls(ed.e, 15, 0, EDIT_CLS_COMMENT, "after undo, far below");
    expect_not_cls(ed.e, 21, 0, EDIT_CLS_COMMENT, "after undo, at the close");
    ET_OK(ed.e, "undo releases the cascade");

    /* もう一度開いて、途中に閉じを足すと、そこで止まる */
    ET_ERR(edit_redo(ed.e), EDIT_OK);
    expect_cls(ed.e, 15, 0, EDIT_CLS_COMMENT, "reopened");
    ET_ERR(edit_goto(ed.e, 10, 1), EDIT_OK);
    ET_ERR(edit_insert(ed.e, "*/", 2), EDIT_OK);
    expect_not_cls(ed.e, 11, 0, EDIT_CLS_COMMENT,
                   "a close on line 10 must release line 11");
    expect_not_cls(ed.e, 15, 0, EDIT_CLS_COMMENT,
                   "a close on line 10 must release line 15");
    expect_cls(ed.e, 9, 0, EDIT_CLS_COMMENT, "line 9 is still inside the comment");
    ET_OK(ed.e, "closing mid-way stops the cascade");

    et_close(&ed);
}

/*
 * キーワード。§A.1 は EDIT_CLS_KEYWORD の一覧を決めていないので、
 * JS の予約語のうち「これが KEYWORD でないなら枠の意味が無い」ものだけ。
 * 一覧そのものは監督への openQuestions に上げてある。
 */
static void t_keywords(void)
{
    et_ed_t ed;
    if (!et_open_default(&ed)) return;
    edit_set_view(ed.e, 40, LEX_ROWS);

    ET_ERR(edit_set_text(ed.e, "function f() {\nvar x = 1;\nreturn x;\n}", 37),
           EDIT_OK);
    expect_cls(ed.e, 1, 0, EDIT_CLS_KEYWORD, "function");
    expect_cls(ed.e, 2, 0, EDIT_CLS_KEYWORD, "var");
    expect_cls(ed.e, 3, 0, EDIT_CLS_KEYWORD, "return");
    /* 識別子は KEYWORD ではない (IDENT か PLAIN かは見ない) */
    expect_not_cls(ed.e, 1, 9, EDIT_CLS_KEYWORD, "the function name");
    /* キーワードを含む長い識別子はキーワードではない */
    ET_ERR(edit_set_text(ed.e, "variable = returnValue;", 23), EDIT_OK);
    expect_not_cls(ed.e, 1, 0, EDIT_CLS_KEYWORD, "\"variable\" starts with \"var\"");
    expect_not_cls(ed.e, 1, 11, EDIT_CLS_KEYWORD,
                   "\"returnValue\" starts with \"return\"");
    ET_OK(ed.e, "keywords");

    et_close(&ed);
}

/* 字句の状態が undo/redo と行の挿入削除に追従すること */
static void t_state_follows_edits(void)
{
    et_ed_t ed;
    if (!et_open_default(&ed)) return;
    edit_set_view(ed.e, 40, LEX_ROWS);

    ET_ERR(edit_set_text(ed.e, "/*\na\nb\n*/\nc", 11), EDIT_OK);
    expect_cls(ed.e, 2, 0, EDIT_CLS_COMMENT, "inside");
    expect_cls(ed.e, 3, 0, EDIT_CLS_COMMENT, "inside");
    expect_not_cls(ed.e, 5, 0, EDIT_CLS_COMMENT, "after the close");
    ET_OK(ed.e, "seeded block comment");

    /* コメントの中に行を足す */
    ET_ERR(edit_goto(ed.e, 2, 2), EDIT_OK);
    ET_ERR(edit_insert(ed.e, "\nz", 2), EDIT_OK);
    ET_CHECK(edit_line_count(ed.e) == 6, "line_count=%u after inserting a line, "
             "want 6", (unsigned)edit_line_count(ed.e));
    expect_cls(ed.e, 3, 0, EDIT_CLS_COMMENT, "the newly inserted line is inside");
    expect_not_cls(ed.e, 6, 0, EDIT_CLS_COMMENT, "still closed at the end");
    ET_OK(ed.e, "insert a line inside a comment");

    /* 閉じの行を消すと、下まで開いたままになる */
    ET_ERR(edit_goto(ed.e, 5, 1), EDIT_OK);
    ET_ERR(edit_delete(ed.e, 2), EDIT_OK);        /* close marker deleted */
    expect_cls(ed.e, 6, 0, EDIT_CLS_COMMENT,
               "deleting the closing marker must extend the comment downwards");
    ET_OK(ed.e, "delete the closing marker");

    /* 戻す */
    ET_ERR(edit_undo(ed.e), EDIT_OK);
    expect_not_cls(ed.e, 6, 0, EDIT_CLS_COMMENT, "undo restored the close");
    ET_OK(ed.e, "undo restores the lexer state");

    et_close(&ed);
}

int main(void)
{
    printf("=== edit_core: lexer ===\n");
    t_single_line();
    t_slash_is_punct();
    t_template_string();
    t_block_comment_cascade();
    t_keywords();
    t_state_follows_edits();
    ET_SUMMARY("test_lex");
    return et_fails ? 1 : 0;
}
