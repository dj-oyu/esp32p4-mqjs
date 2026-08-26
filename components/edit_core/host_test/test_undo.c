/*
 * test_undo.c — undo / redo。§A.1「編集」と「データ構造 (決定)」の undo 節。
 *
 * ここで固定する仕様 (docs/native-editor-spec.md §A.1):
 *  - 「undo はバイト上限のリング: {kind, byte_off, len, text...} の可変長レコード」
 *  - 「**連続する 1 文字挿入は 1 レコードに結合**」
 *  - 「満杯なら最古を捨てて undo_evictions++」
 *  - 「edit_undo: 何も無ければ EDIT_E_STATE」
 *  - 「edit_set_text: 置換、**undo 消去**、カーソル 0」
 *  - 不変条件 5「undo リングのレコード境界が整合し、**redo は最後の undo から連続**」
 *    -> undo したあとに新しい編集をすると redo は消える
 *
 * ---------------------------------------------------------------------------
 * このスイートが見ていないもの
 *
 *  - 結合が「どこで切れるか」。§A.1 は「連続する 1 文字挿入」としか書いて
 *    いない。ここでは (a) 途中に何も挟まない 1 文字挿入の列は 1 レコード、
 *    (b) 複数バイトを一度に入れた挿入は別レコード、の 2 つだけを見る。
 *    カーソル移動・改行・時間経過で切れるかは未決 (openQuestions)
 *  - 削除どうしの結合。§A.1 は挿入についてしか書いていない
 *  - undo が edit_edit_count を進めるかどうか。§A.1 は「単調増加」としか
 *    書いていないので、減らないことしか見ない
 *  - undo 後に edit_modified が false へ戻るか (保存点との関係)。§A.1 に無い
 *  - undo リングのレコード表現そのもの (edit_check() 任せ)
 *  - 選択の undo (選択範囲が復元されるか)。§A.1 に無い
 * ---------------------------------------------------------------------------
 */
#include "edit_test.h"

static uint32_t evictions(const edit_t *e)
{
    edit_stats_t st;
    memset(&st, 0, sizeof(st));
    edit_stats(e, &st);
    return st.undo_evictions;
}

/* 1 回の編集について「undo で戻り、redo で進む」を本文とカーソルの両方で見る */
static void roundtrip(edit_t *e, const char *before_txt, const char *after_txt,
                      const char *what)
{
    edit_pos_t after = edit_cursor(e);
    edit_pos_t back;

    ET_IS(e, after_txt);

    ET_ERR(edit_undo(e), EDIT_OK);
    ET_IS(e, before_txt);
    back = edit_cursor(e);
    ET_OK(e, what);

    ET_ERR(edit_redo(e), EDIT_OK);
    ET_IS(e, after_txt);
    et_checks++;
    if (edit_cursor(e).byte != after.byte) {
        et_fails++;
        printf("FAIL  %s: redo left the cursor at %zu, want %zu\n",
               what, edit_cursor(e).byte, after.byte);
    }
    ET_OK(e, what);

    /* もう一度 undo して、カーソルが同じ場所へ戻ること (往復が安定) */
    ET_ERR(edit_undo(e), EDIT_OK);
    ET_IS(e, before_txt);
    et_checks++;
    if (edit_cursor(e).byte != back.byte) {
        et_fails++;
        printf("FAIL  %s: second undo left the cursor at %zu, first left it at %zu\n",
               what, edit_cursor(e).byte, back.byte);
    }
    ET_ERR(edit_redo(e), EDIT_OK);
    ET_IS(e, after_txt);
    ET_OK(e, what);
}

static void t_roundtrip(void)
{
    et_ed_t ed;
    if (!et_open_default(&ed)) return;

    /* 挿入 */
    ET_ERR(edit_set_text(ed.e, "hello", 5), EDIT_OK);
    ET_ERR(edit_move(ed.e, EDIT_M_DOC_END, 1), EDIT_OK);
    ET_ERR(edit_insert(ed.e, " world", 6), EDIT_OK);
    roundtrip(ed.e, "hello", "hello world", "insert");

    /* 前方削除 */
    ET_ERR(edit_set_text(ed.e, "abcdef", 6), EDIT_OK);
    ET_ERR(edit_goto(ed.e, 1, 3), EDIT_OK);
    ET_ERR(edit_delete(ed.e, 2), EDIT_OK);
    roundtrip(ed.e, "abcdef", "abef", "forward delete");

    /* backspace */
    ET_ERR(edit_set_text(ed.e, "abcdef", 6), EDIT_OK);
    ET_ERR(edit_goto(ed.e, 1, 5), EDIT_OK);
    ET_ERR(edit_delete(ed.e, -2), EDIT_OK);
    roundtrip(ed.e, "abcdef", "abef", "backspace");

    /* 改行の挿入と削除。行数も戻ること */
    ET_ERR(edit_set_text(ed.e, "ab", 2), EDIT_OK);
    ET_ERR(edit_goto(ed.e, 1, 2), EDIT_OK);
    ET_ERR(edit_insert(ed.e, "\n", 1), EDIT_OK);
    ET_CHECK(edit_line_count(ed.e) == 2, "line_count=%u after inserting a newline",
             (unsigned)edit_line_count(ed.e));
    ET_ERR(edit_undo(ed.e), EDIT_OK);
    ET_IS(ed.e, "ab");
    ET_CHECK(edit_line_count(ed.e) == 1, "line_count=%u after undoing the newline, "
             "want 1", (unsigned)edit_line_count(ed.e));
    ET_OK(ed.e, "undo a newline");
    ET_ERR(edit_redo(ed.e), EDIT_OK);
    ET_IS(ed.e, "a\nb");
    ET_CHECK(edit_line_count(ed.e) == 2, "line_count=%u after redo, want 2",
             (unsigned)edit_line_count(ed.e));
    ET_OK(ed.e, "redo a newline");

    /* 全角 */
    ET_ERR(edit_set_text(ed.e, "あ", 3), EDIT_OK);
    ET_ERR(edit_move(ed.e, EDIT_M_DOC_END, 1), EDIT_OK);
    ET_ERR(edit_insert(ed.e, "いう", 6), EDIT_OK);
    roundtrip(ed.e, "あ", "あいう", "wide characters");

    et_close(&ed);
}

/* §A.1「連続する 1 文字挿入は 1 レコードに結合」 */
static void t_coalesce(void)
{
    et_ed_t ed;
    if (!et_open_default(&ed)) return;

    ET_ERR(edit_set_text(ed.e, "", 0), EDIT_OK);
    ET_ERR(edit_insert(ed.e, "a", 1), EDIT_OK);
    ET_ERR(edit_insert(ed.e, "b", 1), EDIT_OK);
    ET_ERR(edit_insert(ed.e, "c", 1), EDIT_OK);
    ET_IS(ed.e, "abc");
    /* 1 レコードなので 1 回の undo で 3 文字ぜんぶ消える */
    ET_ERR(edit_undo(ed.e), EDIT_OK);
    ET_IS(ed.e, "");
    ET_CURSOR(ed.e, 1, 1, 0);
    ET_OK(ed.e, "coalesced single-character inserts");
    /* redo も 1 回で戻る */
    ET_ERR(edit_redo(ed.e), EDIT_OK);
    ET_IS(ed.e, "abc");
    ET_ERR(edit_redo(ed.e), EDIT_E_STATE);
    ET_OK(ed.e, "redo of the coalesced record");

    /* 「1 文字挿入」でないものは結合しない: 複数バイトを一度に入れた挿入は
       それ自体で 1 レコード */
    ET_ERR(edit_set_text(ed.e, "", 0), EDIT_OK);
    ET_ERR(edit_insert(ed.e, "xy", 2), EDIT_OK);
    ET_ERR(edit_insert(ed.e, "zw", 2), EDIT_OK);
    ET_IS(ed.e, "xyzw");
    ET_ERR(edit_undo(ed.e), EDIT_OK);
    ET_IS(ed.e, "xy");
    ET_ERR(edit_undo(ed.e), EDIT_OK);
    ET_IS(ed.e, "");
    ET_ERR(edit_undo(ed.e), EDIT_E_STATE);
    ET_OK(ed.e, "multi-byte inserts are separate records");

    /* 全角 1 文字 (3 バイト) の連打も「1 文字挿入」。バイト数で判定して
       いると、ここが 3 レコードに分かれる。 */
    ET_ERR(edit_set_text(ed.e, "", 0), EDIT_OK);
    ET_ERR(edit_insert(ed.e, "あ", 3), EDIT_OK);
    ET_ERR(edit_insert(ed.e, "い", 3), EDIT_OK);
    ET_ERR(edit_insert(ed.e, "う", 3), EDIT_OK);
    ET_IS(ed.e, "あいう");
    ET_ERR(edit_undo(ed.e), EDIT_OK);
    ET_IS(ed.e, "");
    ET_OK(ed.e, "coalesced single wide characters");

    et_close(&ed);
}

/* §A.1「何も無ければ EDIT_E_STATE」 */
static void t_empty_stack(void)
{
    et_ed_t ed;
    et_snap_t snap;
    if (!et_open_default(&ed)) return;

    /* 開けた直後 */
    ET_ERR(edit_undo(ed.e), EDIT_E_STATE);
    ET_ERR(edit_redo(ed.e), EDIT_E_STATE);
    ET_IS(ed.e, "");
    ET_OK(ed.e, "undo/redo on a fresh editor");

    /* 1 回編集 -> undo は通る、その次は E_STATE */
    ET_ERR(edit_insert(ed.e, "q", 1), EDIT_OK);
    ET_ERR(edit_undo(ed.e), EDIT_OK);
    et_snap(ed.e, &snap);
    ET_ERR(edit_undo(ed.e), EDIT_E_STATE);
    ET_UNCHANGED(ed.e, &snap, "undo past the bottom of the stack");
    ET_OK(ed.e, "undo past the bottom");

    /* redo を使い切ったら E_STATE */
    ET_ERR(edit_redo(ed.e), EDIT_OK);
    et_snap(ed.e, &snap);
    ET_ERR(edit_redo(ed.e), EDIT_E_STATE);
    ET_UNCHANGED(ed.e, &snap, "redo past the top of the stack");
    ET_OK(ed.e, "redo past the top");

    et_close(&ed);
}

/* §A.1「edit_set_text: 置換、undo 消去、カーソル 0」 */
static void t_set_text_clears_undo(void)
{
    et_ed_t ed;
    if (!et_open_default(&ed)) return;

    ET_ERR(edit_insert(ed.e, "one", 3), EDIT_OK);
    ET_ERR(edit_insert(ed.e, "two", 3), EDIT_OK);
    ET_IS(ed.e, "onetwo");

    ET_ERR(edit_set_text(ed.e, "fresh", 5), EDIT_OK);
    /* set_text 自身も undo できない (「undo 消去」) */
    ET_ERR(edit_undo(ed.e), EDIT_E_STATE);
    ET_IS(ed.e, "fresh");
    ET_ERR(edit_redo(ed.e), EDIT_E_STATE);
    ET_IS(ed.e, "fresh");
    ET_OK(ed.e, "set_text clears the undo ring");

    /* undo したあとの set_text でも redo は残らない */
    ET_ERR(edit_move(ed.e, EDIT_M_DOC_END, 1), EDIT_OK);
    ET_ERR(edit_insert(ed.e, "!!", 2), EDIT_OK);
    ET_ERR(edit_undo(ed.e), EDIT_OK);
    ET_IS(ed.e, "fresh");
    ET_ERR(edit_set_text(ed.e, "other", 5), EDIT_OK);
    ET_ERR(edit_redo(ed.e), EDIT_E_STATE);
    ET_IS(ed.e, "other");
    ET_OK(ed.e, "set_text clears redo too");

    et_close(&ed);
}

/* 不変条件 5「redo は最後の undo から連続」 -> 新しい編集で redo は消える */
static void t_new_edit_drops_redo(void)
{
    et_ed_t ed;
    if (!et_open_default(&ed)) return;

    ET_ERR(edit_set_text(ed.e, "base", 4), EDIT_OK);
    ET_ERR(edit_move(ed.e, EDIT_M_DOC_END, 1), EDIT_OK);
    ET_ERR(edit_insert(ed.e, "-A", 2), EDIT_OK);
    ET_ERR(edit_insert(ed.e, "-B", 2), EDIT_OK);
    ET_IS(ed.e, "base-A-B");

    ET_ERR(edit_undo(ed.e), EDIT_OK);
    ET_IS(ed.e, "base-A");
    /* ここで redo できるはずのものがある */
    ET_ERR(edit_redo(ed.e), EDIT_OK);
    ET_IS(ed.e, "base-A-B");
    ET_ERR(edit_undo(ed.e), EDIT_OK);
    ET_IS(ed.e, "base-A");

    /* 新しい編集をすると "-B" は二度と戻らない */
    ET_ERR(edit_insert(ed.e, "-C", 2), EDIT_OK);
    ET_IS(ed.e, "base-A-C");
    ET_ERR(edit_redo(ed.e), EDIT_E_STATE);
    ET_IS(ed.e, "base-A-C");
    ET_OK(ed.e, "a new edit drops the redo branch");

    /* undo は新しい枝をたどる */
    ET_ERR(edit_undo(ed.e), EDIT_OK);
    ET_IS(ed.e, "base-A");
    ET_ERR(edit_undo(ed.e), EDIT_OK);
    ET_IS(ed.e, "base");
    ET_ERR(edit_undo(ed.e), EDIT_E_STATE);
    ET_OK(ed.e, "undo down the new branch");

    et_close(&ed);
}

/* §A.1「満杯なら最古を捨てて undo_evictions++」 */
static void t_ring_evicts(void)
{
    et_ed_t ed;
    edit_config_t cfg = et_cfg();
    int i;
    uint32_t ev0, ev1;
    int undos = 0;

    cfg.undo_bytes = 128;         /* 小さく取って、確実に溢れさせる */
    cfg.max_bytes = 4096;
    cfg.max_lines = 64;
    if (!et_open(&ed, &cfg)) return;

    ET_ERR(edit_set_text(ed.e, "", 0), EDIT_OK);
    ev0 = evictions(ed.e);
    ET_CHECK(ev0 == 0, "undo_evictions=%u on a fresh editor", (unsigned)ev0);

    /* 8 バイトずつ 40 回。1 文字挿入ではないので結合しない = 40 レコード。
       レコードは最低でも本文 8 バイト + ヘッダなので 128 B には収まらない。 */
    for (i = 0; i < 40; i++) {
        ET_ERR(edit_insert(ed.e, "abcdefgh", 8), EDIT_OK);
    }
    ET_CHECK(edit_text_len(ed.e) == 320, "text_len=%zu, want 320",
             edit_text_len(ed.e));
    ET_OK(ed.e, "40 inserts into a 128 B undo ring");

    ev1 = evictions(ed.e);
    ET_CHECK(ev1 > ev0, "undo_evictions=%u after 40 inserts into a %u B ring — "
             "the oldest records should have been dropped",
             (unsigned)ev1, (unsigned)cfg.undo_bytes);

    /* undo を繰り返すと、捨てられたところで E_STATE になって止まる。
       止まった時点の本文は空ではない (最古が復元できないから)。 */
    for (i = 0; i < 100; i++) {
        edit_err_t r = edit_undo(ed.e);
        if (r == EDIT_E_STATE) break;
        et_checks++;
        if (r != EDIT_OK) {
            et_fails++;
            printf("FAIL  undo #%d -> %s\n", i, et_err_name(r));
            break;
        }
        undos++;
    }
    ET_CHECK(undos > 0, "no undo record survived at all in a %u B ring",
             (unsigned)cfg.undo_bytes);
    ET_CHECK(undos < 40, "all 40 records survived in a %u B ring — nothing was "
             "evicted", (unsigned)cfg.undo_bytes);
    ET_CHECK(edit_text_len(ed.e) > 0, "text_len=%zu after undoing everything that "
             "survived — evicted records must not be recoverable",
             edit_text_len(ed.e));
    ET_ERR(edit_undo(ed.e), EDIT_E_STATE);
    ET_OK(ed.e, "undo ring after evictions");

    /* 捨てられた先から redo で戻れる (リングの反対端は無傷) */
    for (i = 0; i < undos; i++) ET_ERR(edit_redo(ed.e), EDIT_OK);
    ET_CHECK(edit_text_len(ed.e) == 320, "text_len=%zu after redoing everything, "
             "want 320", edit_text_len(ed.e));
    ET_ERR(edit_redo(ed.e), EDIT_E_STATE);
    ET_OK(ed.e, "redo after evictions");

    /* 1 レコードが undo_bytes より大きいときも、壊れず・止まらないこと。
       §A.1 はこの場合を決めていないので、返り値は見ない (OK でも
       E_STATE でも通る) — 見るのは不変条件と本文。 */
    {
        char big[512];
        memset(big, 'z', sizeof(big));
        ET_ERR(edit_set_text(ed.e, "", 0), EDIT_OK);
        ET_ERR(edit_insert(ed.e, big, sizeof(big)), EDIT_OK);
        ET_CHECK(edit_text_len(ed.e) == 512, "text_len=%zu, want 512",
                 edit_text_len(ed.e));
        ET_OK(ed.e, "insert larger than the whole undo ring");
        (void)edit_undo(ed.e);
        ET_OK(ed.e, "undo of an oversized record");
    }

    et_close(&ed);
}

/* edit_edit_count は単調増加 (§A.1)。undo でも減らない。 */
static void t_edit_count(void)
{
    et_ed_t ed;
    uint32_t a, b, c;
    if (!et_open_default(&ed)) return;

    a = edit_edit_count(ed.e);
    ET_ERR(edit_insert(ed.e, "x", 1), EDIT_OK);
    b = edit_edit_count(ed.e);
    ET_CHECK(b > a, "edit_count %u -> %u after an insert (must increase)",
             (unsigned)a, (unsigned)b);
    ET_ERR(edit_undo(ed.e), EDIT_OK);
    c = edit_edit_count(ed.e);
    ET_CHECK(c >= b, "edit_count %u -> %u after an undo (must not go backwards)",
             (unsigned)b, (unsigned)c);
    ET_ERR(edit_move(ed.e, EDIT_M_RIGHT, 1), EDIT_OK);
    ET_CHECK(edit_edit_count(ed.e) >= c, "edit_count went backwards on a motion");
    ET_OK(ed.e, "edit_count");

    et_close(&ed);
}

int main(void)
{
    printf("=== edit_core: undo/redo ===\n");
    t_roundtrip();
    t_coalesce();
    t_empty_stack();
    t_set_text_clears_undo();
    t_new_edit_drops_redo();
    t_ring_evicts();
    t_edit_count();
    ET_SUMMARY("test_undo");
    return et_fails ? 1 : 0;
}
