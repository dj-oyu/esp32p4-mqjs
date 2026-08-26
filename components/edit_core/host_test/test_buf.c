/*
 * test_buf.c — 本文バッファの境界。§A.1「本文の出し入れ」と「編集」。
 *
 * ここで固定する仕様 (docs/native-editor-spec.md §A.1):
 *  - edit_set_text: 置換・undo 消去・カーソル 0
 *  - edit_copy_text(off, dst, cap): 「off から cap まで複製。gap を跨いでも連続」
 *  - edit_insert / edit_delete: delete の n は**文字**数、端で clamp して EDIT_OK
 *  - EDIT_E_FULL / EDIT_E_UTF8 は「本文は変更されていない」
 *  - 「本文を変える関数は失敗時に何も変えない (先に検査してから書く)」
 *
 * ---------------------------------------------------------------------------
 * このスイートが見ていないもの
 *
 *  - 行索引 (line_count / 改行の挿入削除) — test_lines.c
 *  - カーソル移動そのもの — test_cursor.c。ここでは gap を動かす手段として
 *    しか使わない
 *  - undo/redo の内容 — test_undo.c。ここは「set_text が undo を消す」の
 *    有無すら見ない
 *  - 選択がある状態の insert (置換) — test_select.c
 *  - copy_text に NULL dst を渡したときの振る舞い。§A.1 が決めていない
 *  - 本文に NUL バイトが入るケース。ET_IS が C 文字列比較なので扱えない。
 *    fuzz_edit.c が扱う
 *  - gap の移動コスト (gap_moves / gap_bytes_moved の値そのもの)。
 *    数える対象の定義が §A.1 に無いので、増えることしか見ない
 *  - max_bytes 丁度のときに undo リングが溢れるかどうか (test_undo.c)
 * ---------------------------------------------------------------------------
 */
#include "edit_test.h"

/* 参照実装: テスト側が持つ「あるべき本文」。gap を持たない素の配列。 */
#define REF_MAX 8192
static char ref[REF_MAX];
static size_t ref_len;

static void ref_set(const char *s, size_t n)
{
    memcpy(ref, s, n);
    ref_len = n;
}

/*
 * §A.1「gap を跨いでも連続」。off と cap の全組み合わせを参照実装と
 * 照合する。gap がどこにあっても結果が同じであることを、gap の位置を
 * 変えながら 3 回まわして確かめる。
 */
static void sweep_copy_text(edit_t *e, const char *where)
{
    static char dst[REF_MAX + 16];
    size_t off, cap;
    size_t len = edit_text_len(e);
    int bad = 0;

    et_checks++;
    if (len != ref_len) {
        et_fails++;
        printf("FAIL  %s: text_len=%zu, want %zu\n", where, len, ref_len);
        return;
    }

    for (off = 0; off <= len + 2; off++) {
        for (cap = 0; cap <= len + 2; cap++) {
            size_t want = 0;
            size_t got;
            if (off < len) {
                want = len - off;
                if (want > cap) want = cap;
            }
            memset(dst, 0x5A, sizeof(dst));
            got = edit_copy_text(e, off, dst, cap);
            if (got != want) {
                if (!bad++) {
                    printf("FAIL  %s: copy_text(off=%zu, cap=%zu) -> %zu, want %zu\n",
                           where, off, cap, got, want);
                }
                continue;
            }
            if (want && memcmp(dst, ref + off, want) != 0) {
                if (!bad++) {
                    printf("FAIL  %s: copy_text(off=%zu, cap=%zu) bytes differ "
                           "(gap seam?)\n", where, off, cap);
                }
            }
            if (dst[want] != 0x5A) {
                if (!bad++) {
                    printf("FAIL  %s: copy_text(off=%zu, cap=%zu) wrote past the "
                           "returned length\n", where, off, cap);
                }
            }
        }
    }
    et_checks++;
    if (bad) {
        et_fails++;
        printf("FAIL  %s: %d copy_text mismatches (first shown above)\n", where, bad);
    }
}

/* ------------------------------------------------------------------------ */

static void t_set_text(void)
{
    et_ed_t ed;
    if (!et_open_default(&ed)) return;

    ET_ERR(edit_set_text(ed.e, "hello", 5), EDIT_OK);
    ET_IS(ed.e, "hello");
    ET_CHECK(edit_text_len(ed.e) == 5, "text_len=%zu", edit_text_len(ed.e));
    /* §A.1「カーソル 0」 */
    ET_CURSOR(ed.e, 1, 1, 0);
    ET_OK(ed.e, "set_text");

    /* 置換であって追記ではない */
    ET_ERR(edit_set_text(ed.e, "bye", 3), EDIT_OK);
    ET_IS(ed.e, "bye");
    ET_CURSOR(ed.e, 1, 1, 0);
    ET_OK(ed.e, "set_text again");

    /* 空にできる */
    ET_ERR(edit_set_text(ed.e, "", 0), EDIT_OK);
    ET_IS(ed.e, "");
    ET_CHECK(edit_text_len(ed.e) == 0, "text_len=%zu after empty set_text",
             edit_text_len(ed.e));
    ET_CURSOR(ed.e, 1, 1, 0);
    ET_OK(ed.e, "empty set_text");

    /* NULL + len 0 は「空にする」と読めるが §A.1 は決めていないので、
       返り値は見ずにクラッシュしないことだけ見る (E_ARG でも OK でもよい)。 */
    (void)edit_set_text(ed.e, NULL, 0);
    ET_OK(ed.e, "set_text(NULL,0)");

    et_close(&ed);
}

static void t_insert_delete(void)
{
    et_ed_t ed;
    if (!et_open_default(&ed)) return;

    ET_ERR(edit_insert(ed.e, "abc", 3), EDIT_OK);
    ET_IS(ed.e, "abc");
    ET_CURSOR(ed.e, 1, 4, 3);
    ET_OK(ed.e, "insert abc");

    /* 途中への挿入 */
    ET_ERR(edit_move(ed.e, EDIT_M_LEFT, 1), EDIT_OK);
    ET_ERR(edit_insert(ed.e, "X", 1), EDIT_OK);
    ET_IS(ed.e, "abXc");
    ET_OK(ed.e, "insert in the middle");

    /* §A.1「n<0: backspace |n| 文字」 */
    ET_ERR(edit_delete(ed.e, -1), EDIT_OK);
    ET_IS(ed.e, "abc");
    ET_OK(ed.e, "backspace");

    /* §A.1「n>0: 前方 n 文字」 */
    ET_ERR(edit_delete(ed.e, 1), EDIT_OK);
    ET_IS(ed.e, "ab");
    ET_OK(ed.e, "forward delete");

    /* §A.1「端で clamp、OK」: 削れる数より多く頼んでもエラーにしない */
    ET_ERR(edit_move(ed.e, EDIT_M_DOC_HOME, 1), EDIT_OK);
    ET_ERR(edit_delete(ed.e, -5), EDIT_OK);
    ET_IS(ed.e, "ab");
    ET_ERR(edit_delete(ed.e, 99), EDIT_OK);
    ET_IS(ed.e, "");
    ET_ERR(edit_delete(ed.e, 3), EDIT_OK);      /* 空でも OK */
    ET_ERR(edit_delete(ed.e, -3), EDIT_OK);
    ET_IS(ed.e, "");
    ET_OK(ed.e, "clamped deletes");

    /* n == 0 は何もしない (「n 文字」の素直な読み) */
    ET_ERR(edit_set_text(ed.e, "abc", 3), EDIT_OK);
    ET_ERR(edit_move(ed.e, EDIT_M_RIGHT, 1), EDIT_OK);
    ET_ERR(edit_delete(ed.e, 0), EDIT_OK);
    ET_IS(ed.e, "abc");
    ET_CURSOR(ed.e, 1, 2, 1);

    et_close(&ed);
}

/* §A.1「n<0: backspace |n| **文字**」。バイトではない。 */
static void t_delete_is_characters(void)
{
    et_ed_t ed;
    if (!et_open_default(&ed)) return;

    /* "あいう" = 3 文字 / 9 バイト */
    ET_ERR(edit_set_text(ed.e, "あいう", 9), EDIT_OK);
    ET_CHECK(edit_text_len(ed.e) == 9, "text_len=%zu, want 9", edit_text_len(ed.e));
    ET_ERR(edit_move(ed.e, EDIT_M_DOC_END, 1), EDIT_OK);
    ET_ERR(edit_delete(ed.e, -1), EDIT_OK);
    ET_IS(ed.e, "あい");
    ET_CHECK(edit_text_len(ed.e) == 6, "text_len=%zu after 1 char backspace, want 6",
             edit_text_len(ed.e));
    ET_OK(ed.e, "backspace over a 3-byte character");

    ET_ERR(edit_move(ed.e, EDIT_M_DOC_HOME, 1), EDIT_OK);
    ET_ERR(edit_delete(ed.e, 1), EDIT_OK);
    ET_IS(ed.e, "い");
    ET_OK(ed.e, "forward delete over a 3-byte character");

    /* 混在。"aあb" から前方 2 文字消すと "b" */
    ET_ERR(edit_set_text(ed.e, "aあb", 5), EDIT_OK);
    ET_ERR(edit_delete(ed.e, 2), EDIT_OK);
    ET_IS(ed.e, "b");
    ET_OK(ed.e, "mixed-width forward delete");

    et_close(&ed);
}

/* §A.1「gap を跨いでも連続」 */
static void t_copy_across_gap(void)
{
    et_ed_t ed;
    char seed[300];
    size_t i;
    if (!et_open_default(&ed)) return;

    for (i = 0; i < sizeof(seed); i++) seed[i] = (char)('0' + (int)(i % 10));
    ET_ERR(edit_set_text(ed.e, seed, sizeof(seed)), EDIT_OK);
    ref_set(seed, sizeof(seed));

    /* gap は set_text 直後どこにあるか決まっていないので、まず素で見る */
    sweep_copy_text(ed.e, "after set_text");

    /* カーソルを真ん中へ動かすと gap もそこへ来る (gap buffer の性質)。
       そこへ 1 文字挿し、gap の**両側**にまたがる読みを全部見る。 */
    ET_ERR(edit_goto(ed.e, 1, 151), EDIT_OK);
    ET_ERR(edit_insert(ed.e, "Z", 1), EDIT_OK);
    memmove(ref + 151, ref + 150, ref_len - 150);
    ref[150] = 'Z';
    ref_len++;
    sweep_copy_text(ed.e, "gap in the middle");
    ET_OK(ed.e, "insert in the middle of 300 bytes");

    /* gap を先頭へ寄せてもう一度 */
    ET_ERR(edit_move(ed.e, EDIT_M_DOC_HOME, 1), EDIT_OK);
    ET_ERR(edit_insert(ed.e, "A", 1), EDIT_OK);
    memmove(ref + 1, ref, ref_len);
    ref[0] = 'A';
    ref_len++;
    sweep_copy_text(ed.e, "gap at the head");

    /* gap を末尾へ寄せてもう一度 */
    ET_ERR(edit_move(ed.e, EDIT_M_DOC_END, 1), EDIT_OK);
    ET_ERR(edit_insert(ed.e, "B", 1), EDIT_OK);
    ref[ref_len++] = 'B';
    sweep_copy_text(ed.e, "gap at the tail");
    ET_OK(ed.e, "gap swept head to tail");

    /* gap を動かした回数は増えているはず (値そのものは仕様に無いので見ない) */
    {
        edit_stats_t st;
        memset(&st, 0, sizeof(st));
        edit_stats(ed.e, &st);
        ET_CHECK(st.gap_moves > 0, "gap_moves=%u after sweeping the cursor across "
                 "300 bytes", (unsigned)st.gap_moves);
    }

    et_close(&ed);
}

/* §A.1「max_bytes 超過 -> EDIT_E_FULL。本文は変更されていない」 */
static void t_full(void)
{
    et_ed_t ed;
    edit_config_t cfg = et_cfg();
    et_snap_t snap;
    char big[64];

    cfg.max_bytes = 32;
    if (!et_open(&ed, &cfg)) return;

    memset(big, 'x', sizeof(big));

    /* 上限ちょうどは入る */
    ET_ERR(edit_set_text(ed.e, big, 32), EDIT_OK);
    ET_CHECK(edit_text_len(ed.e) == 32, "text_len=%zu, want 32", edit_text_len(ed.e));
    ET_OK(ed.e, "set_text at max_bytes");

    /* 1 バイト超えは弾かれ、**本文は 32 バイトのまま** */
    et_snap(ed.e, &snap);
    ET_ERR(edit_set_text(ed.e, big, 33), EDIT_E_FULL);
    ET_UNCHANGED(ed.e, &snap, "set_text over max_bytes");
    ET_OK(ed.e, "rejected set_text");

    /* 満杯からの insert */
    ET_ERR(edit_move(ed.e, EDIT_M_DOC_END, 1), EDIT_OK);
    ET_ERR(edit_insert(ed.e, "y", 1), EDIT_E_FULL);
    ET_UNCHANGED(ed.e, &snap, "insert into a full buffer");
    ET_OK(ed.e, "rejected insert");

    /* 部分的にも入れない (「先に検査してから書く」= 途中まで書いて失敗しない) */
    ET_ERR(edit_set_text(ed.e, big, 20), EDIT_OK);
    et_snap(ed.e, &snap);
    ET_ERR(edit_move(ed.e, EDIT_M_DOC_END, 1), EDIT_OK);
    ET_ERR(edit_insert(ed.e, big, 20), EDIT_E_FULL);
    ET_UNCHANGED(ed.e, &snap, "partial insert must not be applied");
    ET_CHECK(edit_text_len(ed.e) == 20, "text_len=%zu after a rejected 20 B insert "
             "into a 20/32 buffer, want 20", edit_text_len(ed.e));
    ET_OK(ed.e, "rejected partial insert");

    /* 隙間ちょうどは入る */
    ET_ERR(edit_insert(ed.e, big, 12), EDIT_OK);
    ET_CHECK(edit_text_len(ed.e) == 32, "text_len=%zu, want 32", edit_text_len(ed.e));
    ET_OK(ed.e, "insert filling the buffer exactly");

    et_close(&ed);
}

/* §A.1「不正な UTF-8 -> EDIT_E_UTF8。本文は変更されていない」 */
static void t_bad_utf8(void)
{
    et_ed_t ed;
    et_snap_t snap;
    size_t n, i;
    const et_bad_utf8_t *bad = et_bad_utf8(&n);

    if (!et_open_default(&ed)) return;

    for (i = 0; i < n; i++) {
        ET_ERR(edit_set_text(ed.e, "seed", 4), EDIT_OK);
        et_snap(ed.e, &snap);

        /* set_text 経路 */
        et_checks++;
        {
            edit_err_t got = edit_set_text(ed.e, bad[i].bytes, bad[i].len);
            if (got != EDIT_E_UTF8) {
                et_fails++;
                printf("FAIL  set_text(%s) -> %s, want EDIT_E_UTF8\n",
                       bad[i].name, et_err_name(got));
            }
        }
        ET_UNCHANGED(ed.e, &snap, bad[i].name);

        /* insert 経路。カーソルは末尾に置く */
        ET_ERR(edit_move(ed.e, EDIT_M_DOC_END, 1), EDIT_OK);
        et_checks++;
        {
            edit_err_t got = edit_insert(ed.e, bad[i].bytes, bad[i].len);
            if (got != EDIT_E_UTF8) {
                et_fails++;
                printf("FAIL  insert(%s) -> %s, want EDIT_E_UTF8\n",
                       bad[i].name, et_err_name(got));
            }
        }
        ET_UNCHANGED(ed.e, &snap, bad[i].name);
        ET_OK(ed.e, bad[i].name);
    }

    /* 正しい列は当然通る (上の 17 件が「全部弾く」実装で通ってしまわないように) */
    ET_ERR(edit_set_text(ed.e, "a\xC3\xA9\xE6\x97\xA5\xF0\x9F\x98\x80z", 11), EDIT_OK);
    /* 'a' 1 + U+00E9 2 + U+65E5 3 + U+1F600 4 + 'z' 1 = 11 バイト */
    ET_CHECK(edit_text_len(ed.e) == 11, "text_len=%zu for a valid 1/2/3/4-byte mix, "
             "want 11", edit_text_len(ed.e));
    ET_OK(ed.e, "valid utf8 of every length");

    et_close(&ed);
}

/* 挿入した直後の本文が、文字境界の途中で切られていないこと。
   (E_UTF8 を「常に OK」で素通りさせる実装を、別の角度から潰す) */
static void t_boundaries_survive(void)
{
    et_ed_t ed;
    char out[64];
    size_t len, i;
    unsigned cps;
    if (!et_open_default(&ed)) return;

    ET_ERR(edit_set_text(ed.e, "あいうえお", 15), EDIT_OK);
    ET_ERR(edit_goto(ed.e, 1, 5), EDIT_OK);    /* 3 文字目の頭 = セル列 5 */
    ET_ERR(edit_insert(ed.e, "X", 1), EDIT_OK);
    ET_IS(ed.e, "あいXうえお");

    len = et_text(ed.e, out, sizeof(out));
    cps = et_cp_count(out, len);
    ET_CHECK(cps == 6, "codepoints=%u, want 6", cps);
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)out[i];
        if (c >= 0x80 && c < 0xC0) continue;  /* 継続バイト */
        ET_CHECK(c < 0x80 || c >= 0xC2, "byte %zu = 0x%02X is not a valid lead",
                 i, c);
    }
    ET_OK(ed.e, "insert between multi-byte characters");

    et_close(&ed);
}

int main(void)
{
    printf("=== edit_core: buffer ===\n");
    t_set_text();
    t_insert_delete();
    t_delete_is_characters();
    t_copy_across_gap();
    t_full();
    t_bad_utf8();
    t_boundaries_survive();
    ET_SUMMARY("test_buf");
    return et_fails ? 1 : 0;
}
