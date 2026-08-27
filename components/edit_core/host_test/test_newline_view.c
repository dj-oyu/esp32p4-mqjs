/*
 * test_newline_view.c — 改行と行削除のあとに「段が読み出せるか」。
 *
 * 実機で 2026-08-27 に出た形の再現: スクロールは正常なのに、改行や行削除の
 * あと本文が 1 文字も出なくなり、ステータス行だけが画面中央に残る。
 * プレゼンタが読むのは edit_view_row だけなので、そこが空を返しているのか、
 * それとも上 (edit_ui / 提示経路) の問題なのかをここで切り分ける。
 *
 * 見ていないもの: 描画・提示・IME・タッチ。ここは core の契約だけ。
 * 「緑 = エディタが直った」ではない (run_tests.sh の冒頭と同じ断り)。
 */
#include "edit_test.h"

#define COLS 142
#define ROWS 26          /* 本文の段数。ステータス行はプレゼンタ側 */

/* その段に見えている文字を 1 本の文字列に戻す。空段なら "" */
static void row_text(edit_t *e, uint16_t row, char *out, size_t cap)
{
    edit_run_t runs[64];
    char u8[COLS * 4 + 8];
    out[0] = '\0';
    int n = edit_view_row(e, row, runs, 64, u8, sizeof u8);
    if (n <= 0)
        return;
    size_t o = 0;
    for (int i = 0; i < n; i++) {
        size_t len = runs[i].utf8_len;
        if (o + len + 1 > cap)
            break;
        memcpy(out + o, u8 + runs[i].utf8_off, len);
        o += len;
    }
    out[o] = '\0';
}


/* 行を挿入すると、その下の段は「画面上で 1 段ずれる」= 中身が変わる。
   core がそれを dirty に載せているか。載せていなければプレゼンタは
   古い段を描き直さず、画面に古い本文が残る (実機 2026-08-27 の形)。 */
static void t_shift_marks_rows_below(const char *what, int del)
{
    edit_config_t cfg = et_cfg();
    cfg.max_cols = COLS;
    cfg.max_rows = ROWS;
    cfg.max_bytes = 65536;
    cfg.max_lines = 1024;
    et_ed_t ed;
    if (!et_open(&ed, &cfg))
        return;
    edit_t *e = ed.e;
    edit_set_view(e, COLS, ROWS);

    char doc[512];
    int o = 0;
    for (int i = 1; i <= 15; i++)
        o += snprintf(doc + o, sizeof doc - (size_t)o, "line%d\n", i);
    edit_set_text(e, doc, (size_t)o);

    /* 5 行目の行頭。ここで改行/削除すると 5 行目以降が 1 段ずつ動く */
    edit_goto(e, 5, 1);

    char before[ROWS][64];
    for (uint16_t r = 0; r < ROWS; r++)
        row_text(e, r, before[r], sizeof before[r]);

    edit_dirty_clear(e);
    if (del)
        edit_delete(e, -1);      /* 行頭 backspace = 前の行と連結 */
    else
        edit_insert(e, "\n", 1);

    uint32_t fl = edit_dirty_flags(e);
    uint64_t dr = edit_dirty_rows(e);

    for (uint16_t r = 0; r < ROWS; r++) {
        char now[64];
        row_text(e, r, now, sizeof now);
        bool changed = strcmp(before[r], now) != 0;
        bool marked  = (fl & EDIT_DIRTY_ALL) || (dr & ((uint64_t)1 << r));
        ET_CHECK(!changed || marked,
                 "%s: row%u は [%s] -> [%s] と変わったのに dirty に無い "
                 "(flags=%u rows=%llx)",
                 what, r, before[r], now, fl, (unsigned long long)dr);
    }
    et_close(&ed);
}


/* **毎 op、カーソルが画面に居ること。**
 *
 * 実機 (2026-08-27) が出したのはこの形: 改行のあと edit_view_row が 22 段
 * すべてで 0 run を返し、ステータス行だけが描かれた。カーソルが見えていれば
 * その段は必ず 1 run 以上返る (§A.1 の EDIT_RUN_CURSOR) ので、0 run =
 * カーソルが画面外 = 追従スクロールが効いていない。
 *
 * 最初に書いたテストは 40 回改行した「あと」に 1 回だけ見ていて、これを
 * 取り逃した。**途中の状態を見ないテストは、途中で壊れる不具合を通す。** */
static void t_cursor_stays_visible(const char *what, int rows_in_view)
{
    edit_config_t cfg = et_cfg();
    cfg.max_cols = COLS;
    cfg.max_rows = (uint16_t)rows_in_view;
    cfg.max_bytes = 65536;
    cfg.max_lines = 1024;
    et_ed_t ed;
    if (!et_open(&ed, &cfg))
        return;
    edit_t *e = ed.e;
    edit_set_view(e, COLS, (uint16_t)rows_in_view);

    char doc[512];
    int o = 0;
    for (int i = 1; i <= 15; i++)
        o += snprintf(doc + o, sizeof doc - (size_t)o, "line%d\n", i);
    edit_set_text(e, doc, (size_t)o);
    edit_move(e, EDIT_M_DOC_END, 1);

    /* 末尾で改行を続ける = 実機で再現した操作 */
    for (int i = 0; i < 60; i++) {
        edit_insert(e, "\n", 1);

        uint16_t crow = 0, ccol = 0;
        bool vis = edit_cursor_view(e, &crow, &ccol);
        ET_CHECK(vis, "%s: 改行 #%d のあとカーソルが画面外", what, i);
        if (!vis)
            break;

        char t[512];
        edit_run_t runs[64];
        char u8[COLS * 4 + 8];
        int n = edit_view_row(e, crow, runs, 64, u8, sizeof u8);
        ET_CHECK(n > 0,
                 "%s: 改行 #%d のあと、カーソル段 row%u が 0 run "
                 "(実機の rows=0/22 と同じ形)", what, i, crow);
        if (n <= 0)
            break;
        (void)t;
    }

    /* 行削除を続けても同じ */
    for (int i = 0; i < 60; i++) {
        edit_delete(e, -1);

        uint16_t crow = 0, ccol = 0;
        bool vis = edit_cursor_view(e, &crow, &ccol);
        ET_CHECK(vis, "%s: 行削除 #%d のあとカーソルが画面外", what, i);
        if (!vis)
            break;

        edit_run_t runs[64];
        char u8[COLS * 4 + 8];
        int n = edit_view_row(e, crow, runs, 64, u8, sizeof u8);
        ET_CHECK(n > 0, "%s: 行削除 #%d のあと、カーソル段 row%u が 0 run",
                 what, i, crow);
        if (n <= 0)
            break;
    }
    et_close(&ed);
}


/* **スクロールで視界を飛ばしたあとに編集する。**
 *
 * edit_scroll はカーソルを動かさない (§A.1) ので、指でドラッグすると
 * カーソルは画面外に出る。そこで打鍵すると core は追従スクロールで
 * カーソルを引き戻す約束になっている。実機 (2026-08-27) はここで
 * 22 段すべてが 0 run になった —— 引き戻せていない疑い。
 *
 * 「スクロールは問題ないのに、改行すると本文が消える」という報告の順序を
 * そのままなぞる。 */
static void t_edit_after_scroll(int rows_in_view)
{
    edit_config_t cfg = et_cfg();
    cfg.max_cols = COLS;
    cfg.max_rows = (uint16_t)rows_in_view;
    cfg.max_bytes = 65536;
    cfg.max_lines = 1024;
    et_ed_t ed;
    if (!et_open(&ed, &cfg))
        return;
    edit_t *e = ed.e;
    edit_set_view(e, COLS, (uint16_t)rows_in_view);

    char doc[512];
    int o = 0;
    for (int i = 1; i <= 15; i++)
        o += snprintf(doc + o, sizeof doc - (size_t)o, "line%d\n", i);
    edit_set_text(e, doc, (size_t)o);

    static const int deltas[] = { 1, 5, 14, 40, -3, 99 };
    static const char *ops[] = { "kaigyou", "sakujo", "moji" };

    for (unsigned di = 0; di < sizeof deltas / sizeof deltas[0]; di++) {
        for (unsigned oi = 0; oi < 3; oi++) {
            edit_set_text(e, doc, (size_t)o);
            edit_goto(e, 8, 1);              /* 中ほどにカーソルを置く */
            edit_scroll(e, deltas[di]);      /* 指でドラッグ */

            if (oi == 0)      edit_insert(e, "\n", 1);
            else if (oi == 1) edit_delete(e, -1);
            else              edit_insert(e, "x", 1);

            uint16_t crow = 0, ccol = 0;
            bool vis = edit_cursor_view(e, &crow, &ccol);
            ET_CHECK(vis,
                     "scroll(%d) してから %s: カーソルが画面外のまま "
                     "(追従スクロールが引き戻していない)",
                     deltas[di], ops[oi]);
            if (!vis)
                continue;

            int nonempty = 0;
            for (uint16_t r = 0; r < (uint16_t)rows_in_view; r++) {
                edit_run_t runs[64];
                char u8[COLS * 4 + 8];
                if (edit_view_row(e, r, runs, 64, u8, sizeof u8) > 0)
                    nonempty++;
            }
            ET_CHECK(nonempty > 0,
                     "scroll(%d) してから %s: 全 %d 段が 0 run "
                     "(実機の rows=0/22 と同じ形)",
                     deltas[di], ops[oi], rows_in_view);
        }
    }
    et_close(&ed);
}


/* **スクロールで本文を画面から追い出せてはいけない。**
 *
 * 実機 (2026-08-27): 13 行の文書を 22 段のビューで見ているとき、
 * edit_scroll のクランプが top_line ≤ line_count-1 なので
 * 「最終行 1 本だけが見える」ところまで送れる。末尾は空行なので画面は
 * 真っ白になり、そこで改行してもカーソルは top_line の範囲内にあるため
 * 追従スクロールが働かず、空のまま残る —— 「改行すると本文が全部消える」。
 *
 * 約束したいのは「文書に中身があるなら、スクロール後も 1 段は見えている」。
 * 実装の詳細 (top_line の上限式) ではなく、**画面に何が出るか**で書く。 */
static void t_scroll_keeps_content_visible(int rows_in_view)
{
    edit_config_t cfg = et_cfg();
    cfg.max_cols = COLS;
    cfg.max_rows = (uint16_t)rows_in_view;
    cfg.max_bytes = 65536;
    cfg.max_lines = 1024;
    et_ed_t ed;
    if (!et_open(&ed, &cfg))
        return;
    edit_t *e = ed.e;
    edit_set_view(e, COLS, (uint16_t)rows_in_view);

    /* 実機と同じ 13 行 (末尾の改行で 13 行目は空) */
    char doc[512];
    int o = 0;
    for (int i = 1; i <= 12; i++)
        o += snprintf(doc + o, sizeof doc - (size_t)o, "line%d\n", i);

    static const int deltas[] = { 1, 3, 11, 12, 13, 50, 999 };
    for (unsigned di = 0; di < sizeof deltas / sizeof deltas[0]; di++) {
        edit_set_text(e, doc, (size_t)o);
        edit_scroll(e, deltas[di]);

        int nonempty = 0;
        for (uint16_t r = 0; r < (uint16_t)rows_in_view; r++) {
            edit_run_t runs[64];
            char u8[COLS * 4 + 8];
            if (edit_view_row(e, r, runs, 64, u8, sizeof u8) > 0)
                nonempty++;
        }
        ET_CHECK(nonempty > 0,
                 "scroll(%d): 12 行の本文が 1 段も見えない "
                 "(ビュー %d 段。実機の rows=0/22 と同じ形)",
                 deltas[di], rows_in_view);

        /* そこで改行しても空のまま、が実機の症状。 */
        edit_move(e, EDIT_M_DOC_END, 1);
        edit_insert(e, "\n", 1);
        nonempty = 0;
        for (uint16_t r = 0; r < (uint16_t)rows_in_view; r++) {
            edit_run_t runs[64];
            char u8[COLS * 4 + 8];
            if (edit_view_row(e, r, runs, 64, u8, sizeof u8) > 0)
                nonempty++;
        }
        ET_CHECK(nonempty > 0,
                 "scroll(%d) してから改行: 本文が 1 段も見えない", deltas[di]);
    }
    et_close(&ed);
}

int main(void)
{
    edit_config_t cfg = et_cfg();
    cfg.max_cols = COLS;
    cfg.max_rows = ROWS;
    cfg.max_bytes = 65536;
    cfg.max_lines = 1024;
    et_ed_t ed;
    if (!et_open(&ed, &cfg)) {
        ET_SUMMARY("test_newline_view");
        return 1;
    }
    edit_t *e = ed.e;
    edit_set_view(e, COLS, ROWS);
    edit_dirty_clear(e);

    /* 15 行。実機のステータスは "15:1 15lines" だった */
    char doc[512];
    int o = 0;
    for (int i = 1; i <= 15; i++)
        o += snprintf(doc + o, sizeof doc - (size_t)o, "line%d\n", i);
    ET_CHECK(edit_set_text(e, doc, (size_t)o) == EDIT_OK, "set_text");
    ET_CHECK(edit_line_count(e) == 16, "line_count=%u (15 行 + 末尾の空行)",
             edit_line_count(e));

    char t[512];
    row_text(e, 0, t, sizeof t);
    ET_CHECK(strcmp(t, "line1") == 0, "改行前 row0=[%s]", t);

    /* --- 改行 ------------------------------------------------------- */
    edit_move(e, EDIT_M_DOC_END, 1);
    edit_dirty_clear(e);
    ET_CHECK(edit_insert(e, "\n", 1) == EDIT_OK, "insert newline");
    ET_CHECK(edit_check(e), "edit_check after newline");

    uint32_t fl = edit_dirty_flags(e);
    uint64_t dr = edit_dirty_rows(e);
    ET_CHECK(fl != 0 || dr != 0, "改行が dirty を立てない (flags=%u rows=%llx)",
             fl, (unsigned long long)dr);

    row_text(e, 0, t, sizeof t);
    ET_CHECK(strcmp(t, "line1") == 0, "改行後 row0=[%s] (本文が消えた)", t);
    row_text(e, 14, t, sizeof t);
    ET_CHECK(strcmp(t, "line15") == 0, "改行後 row14=[%s]", t);

    /* --- 行削除 (行頭で backspace = 前の行と連結) --------------------- */
    edit_dirty_clear(e);
    ET_CHECK(edit_delete(e, -1) == EDIT_OK, "backspace");
    ET_CHECK(edit_check(e), "edit_check after backspace");
    row_text(e, 0, t, sizeof t);
    ET_CHECK(strcmp(t, "line1") == 0, "削除後 row0=[%s] (本文が消えた)", t);

    /* --- 何度も改行してビューを越えさせる ---------------------------- */
    for (int i = 0; i < 40; i++) {
        ET_CHECK(edit_insert(e, "\n", 1) == EDIT_OK, "insert newline #%d", i);
        ET_CHECK(edit_check(e), "edit_check after newline #%d", i);
    }
    uint16_t crow = 0, ccol = 0;
    ET_CHECK(edit_cursor_view(e, &crow, &ccol),
             "40 回改行したあとカーソルが画面外 (追従スクロールが効いていない)");
    ET_CHECK(crow < ROWS, "cursor row=%u >= ROWS", crow);

    /* 段が 1 つでも読めること。全段空なら本文消失の再現 */
    int nonempty = 0;
    for (uint16_t r = 0; r < ROWS; r++) {
        row_text(e, r, t, sizeof t);
        if (t[0])
            nonempty++;
    }
    ET_CHECK(nonempty > 0, "40 回改行後、全 %d 段が空 (本文消失を core で再現)",
             ROWS);

    et_close(&ed);

    t_shift_marks_rows_below("kaigyou", 0);
    t_shift_marks_rows_below("gyousakujo", 1);
    t_cursor_stays_visible("rows22", 22);   /* 実機と同じ段数 */
    t_cursor_stays_visible("rows26", 26);
    t_edit_after_scroll(22);
    t_scroll_keeps_content_visible(22);
    ET_SUMMARY("test_newline_view");
    return et_fails ? 1 : 0;
}
