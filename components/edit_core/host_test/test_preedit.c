/*
 * test_preedit.c — IME の合成文字列。§A.1「IME の合成文字列」。
 *
 * ここで固定する仕様 (docs/native-editor-spec.md §A.1):
 *  - edit_set_preedit(e, utf8, len): 「バッファには入れない。カーソル位置に
 *    重ねて描くだけ」「len 0 = 消す」「上限 128 B」
 *  - 「preedit は core が持つ 128 B の小バッファ。edit_view_row がカーソル行に
 *    重ねて出す。**本文には入らない**」
 *  - EDIT_CLS_PREEDIT が run のクラスとして存在する
 *
 * なぜここが独立したスイートか (docs/native-editor-design.md §3.1 / 仕様 §A.2):
 * preedit は edit_task が自分の ime_t から毎打鍵渡してくる。ここが本文へ
 * 漏れると、保存したファイルに未確定の読みが混ざる。「本文が変わらない」は
 * 見た目の問題ではなく保存の正しさの問題。
 *
 * ---------------------------------------------------------------------------
 * このスイートが見ていないもの
 *
 *  - 上限を超えたときの**返り値**。§A.1 は「上限 128 B」としか書いておらず
 *    どのエラーかを決めていない。ここでは「EDIT_OK ではない」ことと
 *    「前の preedit も本文も変わらない」ことだけ見る (openQuestions)
 *  - preedit の中の不正な UTF-8 を弾くかどうか。§A.1 に無い
 *  - preedit があるときにカーソルの表示位置が preedit の前か後ろか。
 *    §A.1 に無いので、cursor_view の値は見ない
 *  - preedit 中に insert / delete / undo を呼んだときに preedit が消えるか。
 *    §A.1 に無い (edit_task が毎回 set_preedit し直す設計なので、core 側の
 *    決めは要らないかもしれない)
 *  - 未確定文字列の下線・色そのもの (プレゼンタの担当)
 *  - preedit が画面右端をはみ出すときの折り返し。§A.1 に無い
 * ---------------------------------------------------------------------------
 */
#include "edit_test.h"

/* row 段目の run のうち EDIT_CLS_PREEDIT のものを数え、
   最初のものの col と ncells を返す */
static int count_preedit(const et_row_t *r, uint16_t *first_col, uint16_t *cells)
{
    int n = 0;
    int i;
    unsigned total = 0;
    for (i = 0; i < r->nruns; i++) {
        if (r->runs[i].cls == EDIT_CLS_PREEDIT) {
            if (n == 0 && first_col) *first_col = r->runs[i].col;
            total += r->runs[i].ncells;
            n++;
        }
    }
    if (cells) *cells = (uint16_t)total;
    return n;
}

/* §A.1「本文には入らない」 */
static void t_text_unchanged(void)
{
    et_ed_t ed;
    et_snap_t snap;
    size_t len0;
    if (!et_open_default(&ed)) return;
    edit_set_view(ed.e, 40, 8);

    ET_ERR(edit_set_text(ed.e, "abc\ndef", 7), EDIT_OK);
    ET_ERR(edit_goto(ed.e, 1, 2), EDIT_OK);
    et_snap(ed.e, &snap);
    len0 = edit_text_len(ed.e);

    ET_ERR(edit_set_preedit(ed.e, "かん", 6), EDIT_OK);
    ET_CHECK(edit_text_len(ed.e) == len0, "text_len=%zu with a preedit, want %zu",
             edit_text_len(ed.e), len0);
    ET_UNCHANGED(ed.e, &snap, "set_preedit");
    ET_CURSOR(ed.e, 1, 2, 1);
    ET_OK(ed.e, "preedit does not touch the text");

    /* 保存経路 (copy_text) にも出てこない */
    {
        char buf[64];
        size_t n = edit_copy_text(ed.e, 0, buf, sizeof(buf));
        ET_CHECK(n == len0, "copy_text returned %zu bytes with a preedit, want %zu",
                 n, len0);
        ET_CHECK(n == 7 && memcmp(buf, "abc\ndef", 7) == 0,
                 "copy_text included the preedit");
    }

    /* 行数も変わらない */
    ET_ERR(edit_set_preedit(ed.e, "a\nb", 3), EDIT_OK);
    ET_CHECK(edit_line_count(ed.e) == 2, "line_count=%u with a newline inside the "
             "preedit, want 2", (unsigned)edit_line_count(ed.e));
    ET_UNCHANGED(ed.e, &snap, "preedit containing a newline");
    ET_OK(ed.e, "preedit with a newline");

    /* §A.1「len 0 = 消す」 */
    ET_ERR(edit_set_preedit(ed.e, "", 0), EDIT_OK);
    ET_UNCHANGED(ed.e, &snap, "cleared preedit");
    ET_ERR(edit_set_preedit(ed.e, NULL, 0), EDIT_OK);
    ET_UNCHANGED(ed.e, &snap, "cleared preedit with NULL");
    ET_OK(ed.e, "clearing the preedit");

    et_close(&ed);
}

/* §A.1「edit_view_row がカーソル行に重ねて出す」 */
static void t_shows_on_cursor_row(void)
{
    et_ed_t ed;
    et_row_t r;
    uint16_t crow = 0xFFFF, ccol = 0xFFFF;
    uint16_t pcol = 0xFFFF, pcells = 0;
    unsigned cells_without, cells_with;
    if (!et_open_default(&ed)) return;
    edit_set_view(ed.e, 40, 8);

    ET_ERR(edit_set_text(ed.e, "abcd\nefgh", 9), EDIT_OK);
    ET_ERR(edit_goto(ed.e, 1, 3), EDIT_OK);       /* byte 2、セル列 2 */
    ET_CHECK(edit_cursor_view(ed.e, &crow, &ccol), "cursor is off screen");
    ET_CHECK(crow == 0 && ccol == 2, "cursor_view -> %u,%u, want 0,2",
             (unsigned)crow, (unsigned)ccol);

    /* preedit 無しの段には EDIT_CLS_PREEDIT の run が無い */
    et_row(ed.e, 0, &r);
    ET_ROW_WELLFORMED(&r);
    cells_without = et_row_cells(&r);
    ET_CHECK(cells_without == 4, "row 0 is %u cells without a preedit, want 4",
             cells_without);
    ET_CHECK(count_preedit(&r, NULL, NULL) == 0,
             "a PREEDIT run appeared without any preedit set");

    /* ASCII 2 文字の preedit -> カーソル列から 2 セル */
    ET_ERR(edit_set_preedit(ed.e, "xy", 2), EDIT_OK);
    et_row(ed.e, 0, &r);
    ET_ROW_WELLFORMED(&r);
    ET_CHECK(count_preedit(&r, &pcol, &pcells) > 0,
             "no EDIT_CLS_PREEDIT run on the cursor row");
    ET_CHECK(pcol == ccol, "the preedit run starts at col %u, the cursor is at %u",
             (unsigned)pcol, (unsigned)ccol);
    ET_CHECK(pcells == 2, "the preedit \"xy\" occupies %u cells, want 2",
             (unsigned)pcells);
    cells_with = et_row_cells(&r);
    ET_CHECK(cells_with == cells_without + 2,
             "row 0 is %u cells with a 2-cell preedit, want %u",
             cells_with, cells_without + 2);
    ET_OK(ed.e, "preedit run on the cursor row");

    /* カーソルのいない段には出ない */
    et_row(ed.e, 1, &r);
    ET_ROW_WELLFORMED(&r);
    ET_CHECK(count_preedit(&r, NULL, NULL) == 0,
             "a PREEDIT run appeared on a row without the cursor");
    ET_CHECK(et_row_cells(&r) == 4, "row 1 is %u cells, want 4", et_row_cells(&r));

    /* 全角 2 文字 -> 4 セル (§A.1 の ncells は filler 込み) */
    ET_ERR(edit_set_preedit(ed.e, "かん", 6), EDIT_OK);
    et_row(ed.e, 0, &r);
    ET_ROW_WELLFORMED(&r);
    ET_CHECK(count_preedit(&r, &pcol, &pcells) > 0, "no PREEDIT run for wide text");
    ET_CHECK(pcells == 4, "a 2-character wide preedit occupies %u cells, want 4",
             (unsigned)pcells);
    ET_CHECK(et_row_cells(&r) == cells_without + 4,
             "row 0 is %u cells with a 4-cell preedit, want %u",
             et_row_cells(&r), cells_without + 4);
    ET_OK(ed.e, "wide preedit");

    /* preedit の後ろの本文は右へずれる。列の計算が preedit を数えていること。 */
    {
        int idx = et_run_at(&r, (uint16_t)(ccol + 4));
        ET_CHECK(idx >= 0, "nothing is drawn at col %u — the text after the "
                 "preedit disappeared", (unsigned)(ccol + 4));
        if (idx >= 0) {
            ET_CHECK(r.runs[idx].cls != EDIT_CLS_PREEDIT,
                     "col %u is still part of the preedit run",
                     (unsigned)(ccol + 4));
        }
    }
    ET_OK(ed.e, "text after the preedit is shifted right");

    /* 消すと元の幅に戻る */
    ET_ERR(edit_set_preedit(ed.e, "", 0), EDIT_OK);
    et_row(ed.e, 0, &r);
    ET_ROW_WELLFORMED(&r);
    ET_CHECK(et_row_cells(&r) == cells_without, "row 0 is %u cells after clearing "
             "the preedit, want %u", et_row_cells(&r), cells_without);
    ET_CHECK(count_preedit(&r, NULL, NULL) == 0, "a PREEDIT run survived len 0");
    ET_OK(ed.e, "cleared preedit");

    /* 行頭・行末のカーソルでも出る。
       カーソルを動かしてから preedit を置く: 「移動が preedit を消すか」は
       §A.1 が決めていないので、その順序に依存させない。 */
    ET_ERR(edit_goto(ed.e, 2, 1), EDIT_OK);
    ET_ERR(edit_set_preedit(ed.e, "ab", 2), EDIT_OK);
    et_row(ed.e, 1, &r);
    ET_ROW_WELLFORMED(&r);
    ET_CHECK(count_preedit(&r, &pcol, NULL) > 0, "no PREEDIT run at the start of "
             "a line");
    ET_CHECK(pcol == 0, "the preedit run starts at col %u at the line start",
             (unsigned)pcol);
    ET_ERR(edit_move(ed.e, EDIT_M_END, 1), EDIT_OK);
    ET_ERR(edit_set_preedit(ed.e, "ab", 2), EDIT_OK);
    et_row(ed.e, 1, &r);
    ET_ROW_WELLFORMED(&r);
    ET_CHECK(count_preedit(&r, &pcol, NULL) > 0, "no PREEDIT run at the end of "
             "a line");
    ET_CHECK(pcol == 4, "the preedit run starts at col %u at the end of a 4-cell "
             "line", (unsigned)pcol);
    ET_OK(ed.e, "preedit at the line edges");

    /* 空行にカーソルがあっても出る */
    ET_ERR(edit_set_text(ed.e, "\n", 1), EDIT_OK);
    ET_ERR(edit_goto(ed.e, 1, 1), EDIT_OK);
    ET_ERR(edit_set_preedit(ed.e, "ab", 2), EDIT_OK);
    et_row(ed.e, 0, &r);
    ET_ROW_WELLFORMED(&r);
    ET_CHECK(count_preedit(&r, &pcol, &pcells) > 0,
             "no PREEDIT run on an empty cursor line");
    ET_CHECK(pcells == 2, "the preedit occupies %u cells on an empty line, want 2",
             (unsigned)pcells);
    ET_OK(ed.e, "preedit on an empty line");

    et_close(&ed);
}

/* §A.1「上限 128 B」 */
static void t_limit(void)
{
    et_ed_t ed;
    edit_config_t cfg = et_cfg();
    et_snap_t snap;
    et_row_t r;
    char buf[256];
    uint16_t pcells = 0;
    edit_err_t err;

    /* 128 セルの preedit が右端で切れないだけの幅を取る */
    cfg.max_cols = 200;
    if (!et_open(&ed, &cfg)) return;
    edit_set_view(ed.e, 200, 8);

    ET_ERR(edit_set_text(ed.e, "seed", 4), EDIT_OK);
    ET_ERR(edit_goto(ed.e, 1, 1), EDIT_OK);
    et_snap(ed.e, &snap);

    memset(buf, 'p', sizeof(buf));

    /* 128 B ちょうどは通る */
    ET_ERR(edit_set_preedit(ed.e, buf, 128), EDIT_OK);
    et_row(ed.e, 0, &r);
    ET_ROW_WELLFORMED(&r);
    ET_CHECK(count_preedit(&r, NULL, &pcells) > 0, "no PREEDIT run for 128 bytes");
    ET_CHECK(pcells == 128, "a 128-byte ASCII preedit occupies %u cells, want 128",
             (unsigned)pcells);
    ET_OK(ed.e, "preedit at the 128 B limit");

    /* 129 B は通らない。前の preedit も本文も壊さない。
       返り値は §A.1 が決めていないので「OK ではない」ことだけ見る。 */
    err = edit_set_preedit(ed.e, buf, 129);
    et_checks++;
    if (err == EDIT_OK) {
        et_fails++;
        printf("FAIL  set_preedit(129) -> EDIT_OK; §A.1 caps the preedit at 128 B\n");
    } else {
        printf("note: set_preedit(129) -> %s\n", et_err_name(err));
    }
    ET_UNCHANGED(ed.e, &snap, "oversized preedit");
    et_row(ed.e, 0, &r);
    ET_ROW_WELLFORMED(&r);
    ET_CHECK(count_preedit(&r, NULL, &pcells) > 0,
             "the rejected preedit wiped the previous one");
    ET_CHECK(pcells == 128, "after a rejected 129-byte preedit the run is %u cells, "
             "want the previous 128", (unsigned)pcells);
    ET_OK(ed.e, "rejected oversized preedit");

    /* うんと大きいものでも同じ */
    (void)edit_set_preedit(ed.e, buf, sizeof(buf));
    ET_UNCHANGED(ed.e, &snap, "very large preedit");
    ET_OK(ed.e, "very large preedit");

    /* そのあとも普通に使える */
    ET_ERR(edit_set_preedit(ed.e, "ok", 2), EDIT_OK);
    et_row(ed.e, 0, &r);
    ET_CHECK(count_preedit(&r, NULL, &pcells) > 0 && pcells == 2,
             "preedit is %u cells after the rejected ones, want 2",
             (unsigned)pcells);
    ET_ERR(edit_set_preedit(ed.e, "", 0), EDIT_OK);
    ET_OK(ed.e, "recovered");

    et_close(&ed);
}

int main(void)
{
    printf("=== edit_core: preedit ===\n");
    t_text_unchanged();
    t_shows_on_cursor_row();
    t_limit();
    ET_SUMMARY("test_preedit");
    return et_fails ? 1 : 0;
}
