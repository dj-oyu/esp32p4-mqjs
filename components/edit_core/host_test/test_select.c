/*
 * test_select.c — 選択。§A.1「カーソル・選択」。
 *
 * ここで固定する仕様 (docs/native-editor-spec.md §A.1):
 *  - edit_select_begin: 「アンカー = カーソル」
 *  - edit_select_end:   「選択解除」
 *  - edit_selection(from_byte, to_byte): 選択が無ければ false
 *  - edit_copy_selection(dst, cap)
 *  - edit_delete_selection
 *  - edit_insert: 「選択があれば置換」
 *  - EDIT_E_STATE: 「その状態では不可 (**選択が無いのに delete_selection** 等)」
 *
 * 逆向きの選択 (アンカーがカーソルより後ろ) について: §A.1 は from_byte /
 * to_byte という名前しか与えていない。ここでは **from ≤ to に正規化される**
 * ことを固定する。逆順で返す設計だと copy_selection の長さが負になり、
 * 呼び出し側 (プレゼンタと保存経路) が両方の順序を扱うことになるため。
 *
 * ---------------------------------------------------------------------------
 * このスイートが見ていないもの
 *
 *  - 空の選択 (select_begin 直後、アンカー == カーソル) で edit_selection が
 *    true を返すか false を返すか。§A.1 に無い。true のときは from == to で
 *    あることだけ見る
 *  - カーソル移動が選択を解除するか (shift 押下の有無を core が知らないので、
 *    そもそも core の管轄かが未決)
 *  - 選択の undo (選択範囲そのものが undo で復元されるか) — §A.1 に無い
 *  - 表示側の EDIT_RUN_SELECTED — test_view.c
 *  - 選択したまま set_text したときの選択の行方。§A.1 に無い
 *  - copy_selection に NULL dst を渡したときの振る舞い
 * ---------------------------------------------------------------------------
 */
#include "edit_test.h"

static void expect_sel(const edit_t *e, bool want, size_t from, size_t to,
                       const char *what)
{
    size_t f = (size_t)-1, t = (size_t)-1;
    bool got = edit_selection(e, &f, &t);
    et_checks++;
    if (got != want) {
        et_fails++;
        printf("FAIL  %s: edit_selection -> %s, want %s\n",
               what, got ? "true" : "false", want ? "true" : "false");
        return;
    }
    if (!want) return;
    et_checks++;
    if (f != from || t != to) {
        et_fails++;
        printf("FAIL  %s: selection %zu..%zu, want %zu..%zu\n", what, f, t, from, to);
    }
}

static void expect_copy(const edit_t *e, const char *want, const char *what)
{
    char buf[256];
    size_t w = strlen(want);
    size_t n;

    memset(buf, 0x5A, sizeof(buf));
    n = edit_copy_selection(e, buf, sizeof(buf) - 1);
    et_checks++;
    if (n != w || memcmp(buf, want, w) != 0) {
        buf[n < sizeof(buf) ? n : sizeof(buf) - 1] = 0;
        et_fails++;
        printf("FAIL  %s: copy_selection -> \"%s\" (%zu B), want \"%s\" (%zu B)\n",
               what, buf, n, want, w);
    }
    et_checks++;
    if (buf[n] != 0x5A) {
        et_fails++;
        printf("FAIL  %s: copy_selection wrote past the returned length\n", what);
    }
}

static void t_basic(void)
{
    et_ed_t ed;
    if (!et_open_default(&ed)) return;

    ET_ERR(edit_set_text(ed.e, "hello world", 11), EDIT_OK);

    /* 選択が無い状態 */
    expect_sel(ed.e, false, 0, 0, "fresh editor");
    {
        char buf[8];
        memset(buf, 0x5A, sizeof(buf));
        ET_CHECK(edit_copy_selection(ed.e, buf, sizeof(buf)) == 0,
                 "copy_selection with no selection returned non-zero");
        ET_CHECK(buf[0] == 0x5A, "copy_selection wrote with no selection");
    }
    ET_ERR(edit_delete_selection(ed.e), EDIT_E_STATE);
    ET_IS(ed.e, "hello world");
    ET_OK(ed.e, "no selection");

    /* 順方向: byte 0 から byte 5 まで */
    ET_ERR(edit_goto(ed.e, 1, 1), EDIT_OK);
    edit_select_begin(ed.e);
    ET_ERR(edit_move(ed.e, EDIT_M_RIGHT, 5), EDIT_OK);
    expect_sel(ed.e, true, 0, 5, "forward selection");
    expect_copy(ed.e, "hello", "forward selection");
    ET_OK(ed.e, "forward selection");

    /* select_end で解除 */
    edit_select_end(ed.e);
    expect_sel(ed.e, false, 0, 0, "after select_end");
    ET_ERR(edit_delete_selection(ed.e), EDIT_E_STATE);
    ET_IS(ed.e, "hello world");
    ET_OK(ed.e, "select_end");

    et_close(&ed);
}

/* 逆向き: アンカーがカーソルより後ろ */
static void t_reverse(void)
{
    et_ed_t ed;
    if (!et_open_default(&ed)) return;

    ET_ERR(edit_set_text(ed.e, "hello world", 11), EDIT_OK);
    ET_ERR(edit_goto(ed.e, 1, 12), EDIT_OK);       /* byte 11 = 行末 */
    ET_CURSOR(ed.e, 1, 12, 11);
    edit_select_begin(ed.e);
    ET_ERR(edit_move(ed.e, EDIT_M_LEFT, 5), EDIT_OK);
    ET_CURSOR(ed.e, 1, 7, 6);
    /* from ≤ to に正規化される */
    expect_sel(ed.e, true, 6, 11, "reverse selection");
    expect_copy(ed.e, "world", "reverse selection");
    ET_OK(ed.e, "reverse selection");

    /* 消しても向きに関係なく同じ範囲が消える */
    ET_ERR(edit_delete_selection(ed.e), EDIT_OK);
    ET_IS(ed.e, "hello ");
    ET_CURSOR(ed.e, 1, 7, 6);          /* 消したあとは選択の先頭 */
    expect_sel(ed.e, false, 0, 0, "after delete_selection");
    ET_ERR(edit_delete_selection(ed.e), EDIT_E_STATE);
    ET_IS(ed.e, "hello ");
    ET_OK(ed.e, "delete a reverse selection");

    et_close(&ed);
}

static void t_delete_selection(void)
{
    et_ed_t ed;
    if (!et_open_default(&ed)) return;

    /* 行を跨いだ選択 */
    ET_ERR(edit_set_text(ed.e, "one\ntwo\nthree", 13), EDIT_OK);
    ET_ERR(edit_goto(ed.e, 1, 3), EDIT_OK);       /* byte 2 */
    edit_select_begin(ed.e);
    ET_ERR(edit_goto(ed.e, 3, 3), EDIT_OK);       /* byte 10 */
    expect_sel(ed.e, true, 2, 10, "multi-line selection");
    expect_copy(ed.e, "e\ntwo\nth", "multi-line selection");
    ET_ERR(edit_delete_selection(ed.e), EDIT_OK);
    ET_IS(ed.e, "onree");
    ET_CHECK(edit_line_count(ed.e) == 1, "line_count=%u after deleting two "
             "newlines, want 1", (unsigned)edit_line_count(ed.e));
    ET_CURSOR(ed.e, 1, 3, 2);
    ET_OK(ed.e, "delete across lines");

    /* undo で戻る */
    ET_ERR(edit_undo(ed.e), EDIT_OK);
    ET_IS(ed.e, "one\ntwo\nthree");
    ET_CHECK(edit_line_count(ed.e) == 3, "line_count=%u after undo, want 3",
             (unsigned)edit_line_count(ed.e));
    ET_OK(ed.e, "undo a selection delete");

    /* 全選択 */
    ET_ERR(edit_move(ed.e, EDIT_M_DOC_HOME, 1), EDIT_OK);
    edit_select_begin(ed.e);
    ET_ERR(edit_move(ed.e, EDIT_M_DOC_END, 1), EDIT_OK);
    expect_sel(ed.e, true, 0, 13, "select all");
    ET_ERR(edit_delete_selection(ed.e), EDIT_OK);
    ET_IS(ed.e, "");
    ET_CURSOR(ed.e, 1, 1, 0);
    ET_CHECK(edit_line_count(ed.e) == 1, "line_count=%u after deleting everything",
             (unsigned)edit_line_count(ed.e));
    ET_OK(ed.e, "select all and delete");

    /* 空の本文で select_begin -> delete_selection。選択が空なので E_STATE か、
       何も消さずに OK か。§A.1 が決めていないので返り値は見ず、本文と
       不変条件だけ見る。 */
    edit_select_begin(ed.e);
    (void)edit_delete_selection(ed.e);
    ET_IS(ed.e, "");
    ET_OK(ed.e, "empty selection delete");

    et_close(&ed);
}

/* §A.1「edit_insert: 選択があれば置換」 */
static void t_insert_replaces(void)
{
    et_ed_t ed;
    if (!et_open_default(&ed)) return;

    ET_ERR(edit_set_text(ed.e, "hello world", 11), EDIT_OK);
    ET_ERR(edit_goto(ed.e, 1, 7), EDIT_OK);        /* byte 6 */
    edit_select_begin(ed.e);
    ET_ERR(edit_move(ed.e, EDIT_M_DOC_END, 1), EDIT_OK);
    expect_sel(ed.e, true, 6, 11, "selection to replace");

    ET_ERR(edit_insert(ed.e, "there", 5), EDIT_OK);
    ET_IS(ed.e, "hello there");
    ET_CURSOR(ed.e, 1, 12, 11);
    expect_sel(ed.e, false, 0, 0, "after a replacing insert");
    ET_OK(ed.e, "insert replaces the selection");

    /* 短いものへの置換 */
    ET_ERR(edit_goto(ed.e, 1, 1), EDIT_OK);
    edit_select_begin(ed.e);
    ET_ERR(edit_move(ed.e, EDIT_M_RIGHT, 5), EDIT_OK);
    ET_ERR(edit_insert(ed.e, "hi", 2), EDIT_OK);
    ET_IS(ed.e, "hi there");
    ET_CURSOR(ed.e, 1, 3, 2);
    ET_OK(ed.e, "replace with something shorter");

    /* 逆向きの選択でも置換の結果は同じ */
    ET_ERR(edit_set_text(ed.e, "abcdef", 6), EDIT_OK);
    ET_ERR(edit_goto(ed.e, 1, 5), EDIT_OK);        /* byte 4 */
    edit_select_begin(ed.e);
    ET_ERR(edit_move(ed.e, EDIT_M_LEFT, 2), EDIT_OK);   /* byte 2 */
    expect_sel(ed.e, true, 2, 4, "reverse selection to replace");
    ET_ERR(edit_insert(ed.e, "XY", 2), EDIT_OK);
    ET_IS(ed.e, "abXYef");
    ET_OK(ed.e, "replace a reverse selection");

    /* 置換の undo は 1 回で戻る (削除+挿入の 2 段ではない、という主張は
       §A.1 に無いので、ここでは「いつかは戻る」ことだけ見る) */
    {
        int i;
        for (i = 0; i < 4; i++) {
            if (edit_undo(ed.e) != EDIT_OK) break;
            if (edit_text_len(ed.e) == 6) {
                char got[16];
                size_t n = et_text(ed.e, got, sizeof(got));
                if (n == 6 && memcmp(got, "abcdef", 6) == 0) break;
            }
        }
        ET_IS(ed.e, "abcdef");
        ET_OK(ed.e, "undo a replacement");
    }

    /* 選択があるとき、不正な UTF-8 の insert は選択も消さない
       (「本文を変える関数は失敗時に何も変えない」) */
    ET_ERR(edit_set_text(ed.e, "abcdef", 6), EDIT_OK);
    ET_ERR(edit_goto(ed.e, 1, 2), EDIT_OK);
    edit_select_begin(ed.e);
    ET_ERR(edit_move(ed.e, EDIT_M_RIGHT, 3), EDIT_OK);
    expect_sel(ed.e, true, 1, 4, "selection before a bad insert");
    ET_ERR(edit_insert(ed.e, "\xED\xA0\x80", 3), EDIT_E_UTF8);
    ET_IS(ed.e, "abcdef");
    expect_sel(ed.e, true, 1, 4, "selection after a rejected insert");
    ET_OK(ed.e, "rejected insert leaves the selection alone");

    et_close(&ed);
}

/* copy_selection の cap 境界 */
static void t_copy_cap(void)
{
    et_ed_t ed;
    char buf[32];
    size_t i;
    if (!et_open_default(&ed)) return;

    ET_ERR(edit_set_text(ed.e, "0123456789", 10), EDIT_OK);
    ET_ERR(edit_goto(ed.e, 1, 3), EDIT_OK);       /* byte 2 */
    edit_select_begin(ed.e);
    ET_ERR(edit_goto(ed.e, 1, 9), EDIT_OK);       /* byte 8 */
    expect_sel(ed.e, true, 2, 8, "6-byte selection");

    for (i = 0; i <= 8; i++) {
        size_t want = i < 6 ? i : 6;
        size_t n;
        memset(buf, 0x5A, sizeof(buf));
        n = edit_copy_selection(ed.e, buf, i);
        et_checks++;
        if (n != want) {
            et_fails++;
            printf("FAIL  copy_selection(cap=%zu) -> %zu, want %zu\n", i, n, want);
            continue;
        }
        et_checks++;
        if (n && memcmp(buf, "234567", n) != 0) {
            et_fails++;
            printf("FAIL  copy_selection(cap=%zu) copied the wrong bytes\n", i);
        }
        et_checks++;
        if (buf[n] != 0x5A) {
            et_fails++;
            printf("FAIL  copy_selection(cap=%zu) wrote past %zu bytes\n", i, n);
        }
    }
    ET_OK(ed.e, "copy_selection cap boundary");

    /* gap を選択の内側へ動かしても連続して読める。
       insert が選択を置換してしまわないよう、先に選択を解除する。 */
    edit_select_end(ed.e);
    ET_ERR(edit_goto(ed.e, 1, 6), EDIT_OK);
    ET_ERR(edit_insert(ed.e, "-", 1), EDIT_OK);
    ET_IS(ed.e, "01234-56789");
    ET_ERR(edit_goto(ed.e, 1, 3), EDIT_OK);
    edit_select_begin(ed.e);
    ET_ERR(edit_goto(ed.e, 1, 10), EDIT_OK);
    expect_copy(ed.e, "234-567", "selection spanning the gap");
    ET_OK(ed.e, "selection across the gap");

    et_close(&ed);
}

int main(void)
{
    printf("=== edit_core: selection ===\n");
    t_basic();
    t_reverse();
    t_delete_selection();
    t_insert_replaces();
    t_copy_cap();
    ET_SUMMARY("test_select");
    return et_fails ? 1 : 0;
}
