/*
 * term_hist.h — 履歴ビューの添字だけ。**画素も LVGL も出てこない。**
 *
 * ここを term_ui_tab5.c から出したのは、後ろ向きの走査が off-by-one の
 * 住処だから。あのファイルは丸ごと #ifdef ESP_PLATFORM なのでホストから
 * 触れず、実機でしか確かめられなかった。term_core の公開 API しか使って
 * いない純粋な写像なので、ヘッダに出せばホストで測れる
 * (host_test/test_hist_view.c)。
 */
#pragma once

#include "term_core.h"

/*
 * さかのぼって見ているときの 1 段。
 *
 * 仮想の文書は「スクロールバックの表示行 → 生きているグリッドの行」で、
 * 一番下が最新。下から数えて pos 番目の段が何であるかを返す。
 *   pos < grid_rows        生きている行 (grid_rows-1-pos)
 *   それ以上               履歴のセグメント
 *
 * **論理行と表示行は 1 対 1 ではない。** 1 本の論理行は現在の幅で
 * 何行にも折り返るので、term_core_line_seg_count に聞きながら後ろから
 * 数える。これが「回転しても履歴が正しく出る」根拠でもある (§3.1) ——
 * 折り返しは保存されておらず、読むたびに今の幅で作り直される。
 */
static inline bool hist_row_at(term_core_t *core, int pos, int grid_rows,
                        uint32_t *out_id, int *out_seg)
{
    uint32_t id, first;

    if (pos < grid_rows) {
        *out_id = 0;
        *out_seg = grid_rows - 1 - pos;
        return true;              /* 生きているグリッド */
    }
    pos -= grid_rows;
    first = term_core_sb_first(core);
    id = term_core_sb_end(core);
    while (id > first) {
        int n;
        id--;
        n = term_core_line_seg_count(core, id);
        if (n <= 0)
            continue;             /* 消えた id: 飛ばす */
        if (pos < n) {
            *out_id = id;
            *out_seg = n - 1 - pos;   /* 後ろから数えている */
            return true;
        }
        pos -= n;
    }
    return false;                 /* 履歴を使い切った */
}

/*
 * 履歴が何**表示行**あるか。`want` 行ぶん見つかったら打ち切る。
 *
 * **描く前に上限を知るために要る。** これが無いと、無効な位置で 1 フレーム
 * 描いてから切り詰めることになり、その「上端が空いたフレーム」が画面に
 * 残る —— 端末は入力が無い間フレームを回さないので、誰も描き直さない
 * (実機 2026-08-27 で「数行分下にずれて固定される」として出た)。
 *
 * ついでに速い。段ごとに hist_row_at を呼ぶと走査が段数ぶん重なって
 * O(段数 × 履歴) になるが、上限を先に 1 回求めれば走査は 1 回で済む。
 */
static inline int hist_rows_avail(term_core_t *core, int want)
{
    uint32_t first = term_core_sb_first(core);
    uint32_t id = term_core_sb_end(core);
    int total = 0;

    while (id > first && total < want) {
        int n;
        id--;
        n = term_core_line_seg_count(core, id);
        if (n > 0)
            total += n;
    }
    return total;
}
