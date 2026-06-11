/* ui.cells / ui.scroll の最小スモーク (新 C プリミティブ単体検証用)。
 * 端末ロジック抜きで、既知の内容を数行描いてスクロール 1 回するだけ。
 * クラッシュせず画面に正しいグリフ・色・罫線・反転が出れば C 側 OK。
 * これが通ってから初めて ssh_vt.js の全体を流す (段階テスト)。 */
"use strict";
sys.setAppName("cells_test");

var cs = ui.cellSize();
print("cellSize = " + cs[0] + " x " + cs[1]);

ui.clear(0x0B0E11);
ui.cells(0, 0, "ABCDEFGHIJ abcdefghij 0123456789 !@#$%^&*()", 0xC9D1D9, 0x0B0E11);
ui.cells(0, 1, "red", 0xE05A4E, 0x0B0E11);
ui.cells(4, 1, "green", 0x2ECC71, 0x0B0E11);
ui.cells(10, 1, "blue", 0x4FC3F7, 0x0B0E11);
ui.cells(0, 2, "inverse-bar (fg/bg swap)", 0x0B0E11, 0xFFD479);
ui.cells(0, 3, "box: ┌──┐ │xx│ └──┘",
         0xC9D1D9, 0x0B0E11);
ui.cells(0, 4, "col79 ->", 0x8B98A5, 0x0B0E11);
ui.cells(72, 4, "END", 0xFF6B5E, 0x0B0E11);     /* 80 桁目付近 */
ui.cells(0, 6, "scroll test: this row should move up by 1", 0xC678DD, 0x0B0E11);
ui.scroll(0, 6, 1, 0x0B0E11);                    /* 0..6 を 1 行上へ */
ui.cells(0, 6, "row6 after scroll (was blank)", 0x56B6C2, 0x0B0E11);

print("cells_test drew without crashing");
setInterval(function () {}, 2000); /* 画面を保持 */
