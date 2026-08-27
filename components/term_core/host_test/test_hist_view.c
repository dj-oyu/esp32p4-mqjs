/*
 * test_hist_view.c — 履歴ビューの添字 (term_hist.h)。
 *
 * さかのぼって見ているとき、画面のどの段に何を出すかを決める写像を測る。
 * **折り返しをまたぐ後ろ向きの走査**なので、off-by-one はここに住む。
 * 実機でしか確かめられない場所に置いていたら、ずれ 1 段を目で探すことに
 * なっていた (それを 1 日やった直後なので、ここは先に測る)。
 *
 * 見ていないもの: 画素・blit・タッチ・レジストリ。ここは写像だけ。
 */
#include "test_util.h"
#include "term_hist.h"

/* 仮想の文書は「履歴の表示行 → 生きているグリッド」で、一番下が最新。
   pos は下から数えた番号 (0 = 一番下)。 */

static void case_live_rows(void)
{
    tcore_t t = tc_make(TERM_VT, 20, 4);
    uint32_t id;
    int seg;

    t_case("pos が grid 内なら生きている行、下から数える");
    REQUIRE(t.c != NULL);
    feed(t.c, "a\r\nb\r\nc\r\nd");

    CHK_INT(hist_row_at(t.c, 0, 4, &id, &seg), 1);
    CHK_INT((int)id, 0);              /* 0 = 生きているグリッド */
    CHK_INT(seg, 3);                  /* 一番下 = 最終行 */
    CHK_INT(hist_row_at(t.c, 3, 4, &id, &seg), 1);
    CHK_INT(seg, 0);                  /* 4 段上 = 先頭行 */
    tc_free(&t);
}

static void case_walks_into_history(void)
{
    tcore_t t = tc_make(TERM_VT, 20, 4);
    uint32_t base, id;
    int i, seg;

    t_case("grid を超えた pos は履歴へ、新しい順に降りていく");
    REQUIRE(t.c != NULL);
    base = term_core_sb_end(t.c);
    for (i = 0; i < 10; i++) {
        char b[32];
        sprintf(b, "line%d\r\n", i);
        feed(t.c, b);
    }
    /* 10 行を 4 段に流したので 7 本が履歴へ (line0..line6)。
       折り返しは無いので 1 本 = 1 表示行。 */
    CHK_INT((int)(term_core_sb_end(t.c) - base), 7);

    /* pos=4 は履歴の一番新しい = line6 */
    CHK_INT(hist_row_at(t.c, 4, 4, &id, &seg), 1);
    CHK_INT((int)(id - base), 6);
    CHK_INT(seg, 0);
    /* pos=10 は履歴の一番古い = line0 */
    CHK_INT(hist_row_at(t.c, 10, 4, &id, &seg), 1);
    CHK_INT((int)(id - base), 0);
    CHK_INT(seg, 0);
    /* その先は無い */
    CHK_INT(hist_row_at(t.c, 11, 4, &id, &seg), 0);
    CHK_INT(hist_row_at(t.c, 999, 4, &id, &seg), 0);
    tc_free(&t);
}

static void case_wrapped_lines(void)
{
    tcore_t t = tc_make(TERM_VT, 10, 3);
    uint32_t first, end, id, got_id;
    int i, seg, got_seg, pos, wrapped = 0;

    /* **期待値は前向きに数え直して作る。** 実装は後ろ向きに走るので、
       同じ式を書き写すのではなく別の導出とつき合わせる。 */
    t_case("折り返した論理行の各セグメントが、前向きに数えた位置と一致する");
    REQUIRE(t.c != NULL);
    for (i = 0; i < 8; i++)
        feed(t.c, "ABCDEFGHIJKLMNOPQRSTUVWXY\r\n");

    first = term_core_sb_first(t.c);
    end = term_core_sb_end(t.c);
    REQUIRE(end > first);
    for (id = first; id < end; id++)
        if (term_core_line_seg_count(t.c, id) > 1)
            wrapped = 1;
    CHK_TRUE(wrapped);   /* 折り返した行が履歴に入っている前提のテスト */

    /* 一番新しい履歴行から古い方へ、表示行を 1 つずつ数え上げる。
       pos は grid_rows から始まる (その下は生きているグリッド)。 */
    pos = 3;
    for (id = end; id > first; ) {
        int n;
        id--;
        n = term_core_line_seg_count(t.c, id);
        if (n <= 0)
            continue;
        for (seg = n - 1; seg >= 0; seg--) {
            CHK_INT(hist_row_at(t.c, pos, 3, &got_id, &got_seg), 1);
            CHK_INT((int)got_id, (int)id);
            CHK_INT(got_seg, seg);
            pos++;
        }
    }
    /* 数え上げた先はもう無い */
    CHK_INT(hist_row_at(t.c, pos, 3, &got_id, &got_seg), 0);
    tc_free(&t);
}

static void case_contiguous(void)
{
    tcore_t t = tc_make(TERM_VT, 10, 3);
    uint32_t id, prev_id = 0;
    int seg, prev_seg = -1, pos, first = 1;

    t_case("pos を 1 ずつ上げると、表示行は隙間も重複も無く 1 つずつ戻る");
    REQUIRE(t.c != NULL);
    {
        int i;
        for (i = 0; i < 6; i++)
            feed(t.c, "ABCDEFGHIJKLMNOPQRSTUVWXY\r\n");
    }
    for (pos = 0; pos < 60; pos++) {
        if (!hist_row_at(t.c, pos, 3, &id, &seg))
            break;
        if (!first && id == prev_id)
            CHK_INT(seg, prev_seg - 1);      /* 同じ論理行なら 1 つ手前 */
        else if (!first)
            CHK_INT(prev_seg, 0);            /* 行が変わる = 手前は seg 0 */
        prev_id = id;
        prev_seg = seg;
        first = 0;
    }
    CHK_TRUE(pos > 3);   /* 少なくとも履歴に入っている */
    tc_free(&t);
}


/* hist_rows_avail: 履歴の表示行数。**描く前の切り詰めがこれに乗る**ので、
   want で打ち切ったときも「あるだけ」を正しく返すこと。 */
static void case_rows_avail(void)
{
    tcore_t t = tc_make(TERM_VT, 10, 3);
    int i, total, id_total = 0;
    uint32_t id, first, end;

    t_case("hist_rows_avail は履歴の表示行数を返し、want で打ち切れる");
    REQUIRE(t.c != NULL);
    for (i = 0; i < 8; i++)
        feed(t.c, "ABCDEFGHIJKLMNOPQRSTUVWXY\r\n");

    first = term_core_sb_first(t.c);
    end = term_core_sb_end(t.c);
    for (id = first; id < end; id++) {
        int n = term_core_line_seg_count(t.c, id);
        if (n > 0)
            id_total += n;
    }

    /* want が十分大きければ全部数える */
    total = hist_rows_avail(t.c, id_total + 100);
    CHK_INT(total, id_total);
    /* want で打ち切っても、want 以上あることは分かる */
    CHK_TRUE(hist_rows_avail(t.c, 1) >= 1);
    CHK_TRUE(hist_rows_avail(t.c, id_total) >= id_total);

    /* **上限そのもの**: avail 行ちょうどまでは描け、その 1 つ先は無い。
       描く前の切り詰めが正しいことは、この境界がすべて。 */
    {
        uint32_t gid;
        int gseg;
        CHK_INT(hist_row_at(t.c, 3 + id_total - 1, 3, &gid, &gseg), 1);
        CHK_INT(hist_row_at(t.c, 3 + id_total, 3, &gid, &gseg), 0);
    }
    tc_free(&t);
}

int main(void)
{
    t_suite("hist-view");
    case_live_rows();
    case_walks_into_history();
    case_wrapped_lines();
    case_contiguous();
    case_rows_avail();
    return t_summary();
}
