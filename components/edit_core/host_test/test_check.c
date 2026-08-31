/*
 * test_check.c — edit_check() が長い操作列のあいだ真であり続けること。
 * §A.1「不変条件 (edit_check が全部見る)」の 6 項目。
 *
 *   1. 0 ≤ gap_begin ≤ gap_end ≤ cap、本文長 = cap − (gap_end − gap_begin)
 *   2. line_start[0] == 0、各 line_start[i] は改行の直後、line_count 個で
 *      本文を尽くす。gap 内を指す line_start は無い
 *   3. カーソルの byte は文字境界、line1/col1 は byte から再導出した値と一致
 *   4. line_state[i] は line_state[i-1] から行 i-1 を字句解析した結果と一致
 *   5. undo リングのレコード境界が整合し、redo は最後の undo から連続
 *   6. gap の両端 8 バイトのカナリアが無傷
 *
 * ここは**緑の側だけ**を担当する。各不変条件を狙って壊したときに
 * edit_check() が false を返すかどうかは敵対的検証エージェントの担当
 * (docs/native-editor-plan.md §1「検証物は、それが名指しする欠陥を注入して
 * 落ちることを見せるまで、通ったことにしない」)。
 * したがって **このスイートが緑であることは edit_check が正しいことの
 * 証明にはならない。** `return true;` だけの edit_check でも通る。
 *
 * それでも要る理由: 6 つの不変条件を**それぞれ動かす**操作列を通すこと自体が
 * 回帰の網になる。gap を跨ぐ削除、gap 内を指す行索引、字句状態の波及、
 * undo リングの巻き — どれも「たまたま試さなかった」で抜けやすい。
 *
 * ---------------------------------------------------------------------------
 * このスイートが見ていないもの
 *
 *  - edit_check() の**検出能力**。上記のとおり赤い側は敵対的検証の担当
 *  - どの不変条件が破れたかの内訳。edit_check は bool しか返さない
 *  - 乱数による長時間の探索 — fuzz_edit.c の担当
 *  - edit_check 自身の実行時間。§A.1 は「打鍵経路では呼ばない」としか
 *    書いておらず上限を決めていない
 * ---------------------------------------------------------------------------
 */
#include "edit_test.h"

/* 不変条件 1 と 6: gap をあちこちへ動かし、両端すれすれで編集する */
static void t_inv1_gap(void)
{
    et_ed_t ed;
    edit_config_t cfg = et_cfg();
    char seed[400];
    size_t i;

    cfg.max_bytes = 512;
    if (!et_open(&ed, &cfg)) return;

    for (i = 0; i < sizeof(seed); i++) seed[i] = (char)('a' + (int)(i % 26));
    ET_ERR(edit_set_text(ed.e, seed, sizeof(seed)), EDIT_OK);
    ET_OK(ed.e, "set_text 400 B");

    /* 先頭 -> 末尾 -> 先頭 と gap を掃く */
    ET_ERR(edit_move(ed.e, EDIT_M_DOC_HOME, 1), EDIT_OK);
    ET_ERR(edit_insert(ed.e, "<", 1), EDIT_OK);
    ET_OK(ed.e, "insert at byte 0");
    ET_ERR(edit_move(ed.e, EDIT_M_DOC_END, 1), EDIT_OK);
    ET_ERR(edit_insert(ed.e, ">", 1), EDIT_OK);
    ET_OK(ed.e, "insert at the end");
    ET_ERR(edit_move(ed.e, EDIT_M_DOC_HOME, 1), EDIT_OK);
    ET_ERR(edit_delete(ed.e, 1), EDIT_OK);
    ET_OK(ed.e, "delete at byte 0");
    ET_ERR(edit_move(ed.e, EDIT_M_DOC_END, 1), EDIT_OK);
    ET_ERR(edit_delete(ed.e, -1), EDIT_OK);
    ET_OK(ed.e, "delete at the end");

    /* 満杯まで詰めてから空にする (gap が 0 幅になる / 全部 gap になる) */
    {
        char fill[112];
        memset(fill, 'z', sizeof(fill));
        ET_ERR(edit_insert(ed.e, fill, sizeof(fill)), EDIT_OK);
        ET_CHECK(edit_text_len(ed.e) == 512, "text_len=%zu, want max_bytes 512",
                 edit_text_len(ed.e));
        ET_OK(ed.e, "buffer completely full (gap width 0)");
        ET_ERR(edit_insert(ed.e, "!", 1), EDIT_E_FULL);
        ET_OK(ed.e, "rejected insert on a zero-width gap");
    }
    ET_ERR(edit_move(ed.e, EDIT_M_DOC_HOME, 1), EDIT_OK);
    ET_ERR(edit_delete(ed.e, 10000), EDIT_OK);
    ET_IS(ed.e, "");
    ET_OK(ed.e, "buffer completely empty (gap is everything)");

    /* 空から 1 文字ずつ 200 回 */
    for (i = 0; i < 200; i++) {
        char c = (char)('0' + (int)(i % 10));
        ET_ERR(edit_insert(ed.e, &c, 1), EDIT_OK);
    }
    ET_CHECK(edit_text_len(ed.e) == 200, "text_len=%zu, want 200",
             edit_text_len(ed.e));
    ET_OK(ed.e, "200 single-character inserts");

    /* 途中へ潜り込んでの挿入と削除を往復させる */
    for (i = 0; i < 40; i++) {
        ET_ERR(edit_goto(ed.e, 1, (uint32_t)(1 + (i * 7) % 150)), EDIT_OK);
        ET_ERR(edit_insert(ed.e, "#", 1), EDIT_OK);
        ET_ERR(edit_delete(ed.e, -1), EDIT_OK);
    }
    ET_CHECK(edit_text_len(ed.e) == 200, "text_len=%zu after 40 insert/delete "
             "round trips, want 200", edit_text_len(ed.e));
    ET_OK(ed.e, "gap swept back and forth");

    et_close(&ed);
}

/* 不変条件 2: 行索引。gap の中に行頭が入り込む形を狙う */
static void t_inv2_lines(void)
{
    et_ed_t ed;
    int i;
    if (!et_open_default(&ed)) return;

    ET_ERR(edit_set_text(ed.e, "", 0), EDIT_OK);
    ET_OK(ed.e, "empty");

    /* 改行だけを 30 本 */
    for (i = 0; i < 30; i++) {
        ET_ERR(edit_insert(ed.e, "\n", 1), EDIT_OK);
        ET_OK(ed.e, "insert newline");
    }
    ET_CHECK(edit_line_count(ed.e) == 31, "line_count=%u, want 31",
             (unsigned)edit_line_count(ed.e));

    /* 行の途中で分割し、また合流させる。gap は分割点に居座る */
    ET_ERR(edit_set_text(ed.e, "aaaa\nbbbb\ncccc", 14), EDIT_OK);
    for (i = 0; i < 20; i++) {
        ET_ERR(edit_goto(ed.e, 2, 3), EDIT_OK);
        ET_ERR(edit_insert(ed.e, "\n", 1), EDIT_OK);
        ET_CHECK(edit_line_count(ed.e) == 4, "line_count=%u after splitting, want 4",
                 (unsigned)edit_line_count(ed.e));
        ET_OK(ed.e, "split");
        ET_ERR(edit_delete(ed.e, -1), EDIT_OK);
        ET_CHECK(edit_line_count(ed.e) == 3, "line_count=%u after joining, want 3",
                 (unsigned)edit_line_count(ed.e));
        ET_OK(ed.e, "join");
    }
    ET_IS(ed.e, "aaaa\nbbbb\ncccc");

    /* 先頭行と最終行を消す */
    ET_ERR(edit_goto(ed.e, 1, 1), EDIT_OK);
    ET_ERR(edit_delete(ed.e, 5), EDIT_OK);
    ET_IS(ed.e, "bbbb\ncccc");
    ET_OK(ed.e, "delete the first line");
    ET_ERR(edit_move(ed.e, EDIT_M_DOC_END, 1), EDIT_OK);
    ET_ERR(edit_delete(ed.e, -5), EDIT_OK);
    ET_IS(ed.e, "bbbb");
    ET_CHECK(edit_line_count(ed.e) == 1, "line_count=%u, want 1",
             (unsigned)edit_line_count(ed.e));
    ET_OK(ed.e, "delete the last line");

    et_close(&ed);
}

/* 不変条件 3: カーソルは常に文字境界。全角と改行を混ぜて歩き回る */
static void t_inv3_cursor(void)
{
    et_ed_t ed;
    const char *doc = "aあb\nいc\n\nうえお";
    size_t len;
    int i;
    if (!et_open_default(&ed)) return;

    len = strlen(doc);
    ET_ERR(edit_set_text(ed.e, doc, len), EDIT_OK);
    ET_OK(ed.e, "mixed-width document");

    /* 右へ全部、左へ全部 */
    for (i = 0; i < 40; i++) {
        ET_ERR(edit_move(ed.e, EDIT_M_RIGHT, 1), EDIT_OK);
        ET_OK(ed.e, "right");
    }
    ET_CHECK(edit_cursor(ed.e).byte == len, "cursor byte %zu, want %zu",
             edit_cursor(ed.e).byte, len);
    for (i = 0; i < 40; i++) {
        ET_ERR(edit_move(ed.e, EDIT_M_LEFT, 1), EDIT_OK);
        ET_OK(ed.e, "left");
    }
    ET_CURSOR(ed.e, 1, 1, 0);

    /* 上下 + HOME/END を混ぜる */
    for (i = 0; i < 30; i++) {
        static const edit_motion_t ms[] = {
            EDIT_M_DOWN, EDIT_M_END, EDIT_M_UP, EDIT_M_HOME,
            EDIT_M_RIGHT, EDIT_M_WORD_RIGHT, EDIT_M_WORD_LEFT, EDIT_M_PGDN,
            EDIT_M_PGUP, EDIT_M_DOC_END, EDIT_M_DOC_HOME
        };
        ET_ERR(edit_move(ed.e, ms[i % (int)(sizeof(ms) / sizeof(ms[0]))], 1),
               EDIT_OK);
        ET_OK(ed.e, "mixed motion");
        ET_CHECK(edit_cursor(ed.e).byte <= len, "cursor byte %zu past the end",
                 edit_cursor(ed.e).byte);
    }

    /* 全角の直前・直後での削除 */
    ET_ERR(edit_goto(ed.e, 1, 2), EDIT_OK);
    ET_ERR(edit_delete(ed.e, 1), EDIT_OK);       /* 'あ' を消す */
    ET_IS(ed.e, "ab\nいc\n\nうえお");
    ET_OK(ed.e, "delete a wide character forwards");
    ET_ERR(edit_goto(ed.e, 2, 3), EDIT_OK);
    ET_ERR(edit_delete(ed.e, -1), EDIT_OK);      /* 'い' を消す */
    ET_IS(ed.e, "ab\nc\n\nうえお");
    ET_OK(ed.e, "delete a wide character backwards");

    et_close(&ed);
}

/* 不変条件 4: 字句状態。開いて閉じてを繰り返す */
static void t_inv4_lex(void)
{
    et_ed_t ed;
    int i;
    if (!et_open_default(&ed)) return;
    edit_set_view(ed.e, 40, 8);

    ET_ERR(edit_set_text(ed.e,
        "var a = 1;\nvar b = 2;\nvar c = 3;\nvar d = 4;\n"
        "var e = 5;\nvar f = 6;\nvar g = 7;\nvar h = 8;", 87), EDIT_OK);
    ET_CHECK(edit_line_count(ed.e) == 8, "line_count=%u, want 8",
             (unsigned)edit_line_count(ed.e));
    ET_OK(ed.e, "plain source");

    for (i = 0; i < 10; i++) {
        et_row_t r;

        /* ブロックコメントを開く */
        ET_ERR(edit_goto(ed.e, 2, 1), EDIT_OK);
        ET_ERR(edit_insert(ed.e, "/*", 2), EDIT_OK);
        ET_OK(ed.e, "open a block comment");
        et_row(ed.e, 5, &r);            /* 波及先を 1 段読む (遅延を解かせる) */
        ET_ROW_WELLFORMED(&r);
        ET_OK(ed.e, "view a cascaded row");

        /* 閉じる */
        ET_ERR(edit_goto(ed.e, 7, 1), EDIT_OK);
        ET_ERR(edit_insert(ed.e, "*/", 2), EDIT_OK);
        ET_OK(ed.e, "close the block comment");
        et_row(ed.e, 7, &r);
        ET_ROW_WELLFORMED(&r);

        /* テンプレート文字列も開いて閉じる */
        ET_ERR(edit_goto(ed.e, 8, 1), EDIT_OK);
        ET_ERR(edit_insert(ed.e, "`", 1), EDIT_OK);
        ET_OK(ed.e, "open a template string");
        ET_ERR(edit_insert(ed.e, "`", 1), EDIT_OK);
        ET_OK(ed.e, "close the template string");

        /* 全部戻す */
        while (edit_undo(ed.e) == EDIT_OK) {
            ET_OK(ed.e, "undo");
        }
        ET_IS(ed.e,
              "var a = 1;\nvar b = 2;\nvar c = 3;\nvar d = 4;\n"
              "var e = 5;\nvar f = 6;\nvar g = 7;\nvar h = 8;");
    }
    ET_OK(ed.e, "ten open/close cycles");

    et_close(&ed);
}

/* 不変条件 5: undo リング。巻いたあとに undo と redo を往復する */
static void t_inv5_undo(void)
{
    et_ed_t ed;
    edit_config_t cfg = et_cfg();
    int i, round;

    cfg.undo_bytes = 256;
    if (!et_open(&ed, &cfg)) return;

    for (round = 0; round < 5; round++) {
        ET_ERR(edit_set_text(ed.e, "", 0), EDIT_OK);
        ET_OK(ed.e, "set_text clears the ring");

        for (i = 0; i < 30; i++) {
            ET_ERR(edit_insert(ed.e, "0123456789", 10), EDIT_OK);
            ET_OK(ed.e, "insert");
        }
        /* 巻いているところで undo と redo を交互に */
        for (i = 0; i < 20; i++) {
            (void)edit_undo(ed.e);
            ET_OK(ed.e, "undo");
            (void)edit_redo(ed.e);
            ET_OK(ed.e, "redo");
            (void)edit_undo(ed.e);
            ET_OK(ed.e, "undo again");
        }
        /* 底まで undo */
        while (edit_undo(ed.e) == EDIT_OK) {
            ET_OK(ed.e, "undo to the bottom");
        }
        ET_ERR(edit_undo(ed.e), EDIT_E_STATE);
        /* 天井まで redo */
        while (edit_redo(ed.e) == EDIT_OK) {
            ET_OK(ed.e, "redo to the top");
        }
        ET_ERR(edit_redo(ed.e), EDIT_E_STATE);
        ET_CHECK(edit_text_len(ed.e) == 300, "text_len=%zu after undoing and "
                 "redoing everything, want 300", edit_text_len(ed.e));
        ET_OK(ed.e, "ring round trip");
    }

    et_close(&ed);
}

/* 保存フラグ。§A.1「edit_modified / edit_mark_saved」 */
static void t_modified(void)
{
    et_ed_t ed;
    if (!et_open_default(&ed)) return;

    ET_ERR(edit_set_text(ed.e, "body", 4), EDIT_OK);
    edit_mark_saved(ed.e);
    ET_CHECK(!edit_modified(ed.e), "modified right after mark_saved");
    ET_OK(ed.e, "mark_saved");

    ET_ERR(edit_move(ed.e, EDIT_M_RIGHT, 1), EDIT_OK);
    ET_CHECK(!edit_modified(ed.e), "a cursor motion set the modified flag");

    ET_ERR(edit_insert(ed.e, "x", 1), EDIT_OK);
    ET_CHECK(edit_modified(ed.e), "an insert did not set the modified flag");
    ET_OK(ed.e, "insert marks modified");

    edit_mark_saved(ed.e);
    ET_CHECK(!edit_modified(ed.e), "modified after saving again");

    ET_ERR(edit_delete(ed.e, -1), EDIT_OK);
    ET_CHECK(edit_modified(ed.e), "a delete did not set the modified flag");
    ET_OK(ed.e, "delete marks modified");

    /* preedit は本文を変えないので modified を立てない */
    edit_mark_saved(ed.e);
    ET_ERR(edit_set_preedit(ed.e, "ab", 2), EDIT_OK);
    ET_CHECK(!edit_modified(ed.e), "a preedit set the modified flag — the preedit "
             "is not part of the text (§A.1)");
    ET_ERR(edit_set_preedit(ed.e, "", 0), EDIT_OK);
    ET_OK(ed.e, "preedit does not mark modified");

    et_close(&ed);
}

/* 6 つ全部を 1 本の長い列で混ぜる */
static void t_long_mixed_run(void)
{
    et_ed_t ed;
    edit_config_t cfg = et_cfg();
    int i;
    static const char *frag[] = {
        "var ", "x", " = ", "1", ";\n", "/* ", "note", " */\n",
        "`tpl\n", "more`;\n", "\"str\"", "// tail\n", "あ", "日本語\n",
    };
    const int nfrag = (int)(sizeof(frag) / sizeof(frag[0]));

    cfg.max_bytes = 4096;
    cfg.max_lines = 64;
    cfg.undo_bytes = 512;
    cfg.max_cols = 40;
    cfg.max_rows = 8;
    if (!et_open(&ed, &cfg)) return;
    edit_set_view(ed.e, 40, 8);

    for (i = 0; i < 300; i++) {
        const char *f = frag[i % nfrag];
        edit_err_t err;

        switch (i % 7) {
        case 0:
            err = edit_insert(ed.e, f, strlen(f));
            ET_CHECK(err == EDIT_OK || err == EDIT_E_FULL || err == EDIT_E_LINES,
                     "insert -> %s", et_err_name(err));
            break;
        case 1:
            ET_ERR(edit_move(ed.e, (edit_motion_t)(i % 12), 1), EDIT_OK);
            break;
        case 2:
            ET_ERR(edit_delete(ed.e, (i % 3) ? 1 : -1), EDIT_OK);
            break;
        case 3:
            edit_select_begin(ed.e);
            ET_ERR(edit_move(ed.e, EDIT_M_WORD_RIGHT, 1), EDIT_OK);
            (void)edit_delete_selection(ed.e);
            break;
        case 4:
            (void)edit_undo(ed.e);
            break;
        case 5:
            ET_ERR(edit_goto(ed.e, (uint32_t)(1 + i % 9), (uint32_t)(1 + i % 5)),
                   EDIT_OK);
            break;
        default: {
            et_row_t r;
            et_row(ed.e, (uint16_t)(i % 8), &r);
            ET_ROW_WELLFORMED(&r);
            (void)edit_scroll(ed.e, (i % 2) ? 1 : -1);
            break;
        }
        }
        ET_OK(ed.e, "mixed run");
        ET_CHECK(edit_text_len(ed.e) <= cfg.max_bytes,
                 "text_len=%zu over max_bytes %u", edit_text_len(ed.e),
                 (unsigned)cfg.max_bytes);
        ET_CHECK(edit_line_count(ed.e) <= cfg.max_lines,
                 "line_count=%u over max_lines %u",
                 (unsigned)edit_line_count(ed.e), (unsigned)cfg.max_lines);
        ET_CHECK(edit_cursor(ed.e).byte <= edit_text_len(ed.e),
                 "cursor byte %zu past the end %zu",
                 edit_cursor(ed.e).byte, edit_text_len(ed.e));
    }
    ET_OK(ed.e, "300 mixed operations");

    et_close(&ed);
}

int main(void)
{
    printf("=== edit_core: invariants ===\n");
    t_inv1_gap();
    t_inv2_lines();
    t_inv3_cursor();
    t_inv4_lex();
    t_inv5_undo();
    t_modified();
    t_long_mixed_run();
    ET_SUMMARY("test_check");
    return et_fails ? 1 : 0;
}
