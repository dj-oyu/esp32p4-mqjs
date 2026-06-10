/* Tab5 タッチデモ (Phase 3 受け入れ): JS だけでお絵かき。
 * なぞると線、指を上げるたびに色が変わる。右上の赤い四角でクリア。
 * PC では onTouch が発火しないので print だけ。 */
"use strict";

var sz = ui.size();
var W = sz[0], H = sz[1];
if (!W)
    print("ui: この機体に画面はありません");

var BG = 0x101820;
var COLORS = [0x4FC3F7, 0x2ECC71, 0xFFD479, 0xE05A4E, 0xC678DD, 0xFFFFFF];
var ci = 0;
var px = -1, py = -1;

function scene() {
    ui.clear(BG);
    ui.text(16, 12, "タッチデモ: なぞって描く / 右上の □ でクリア", 0xE0E6EA);
    ui.rect(W - 72, 8, 56, 56, 0xE05A4E);
}
scene();

ui.onTouch(function (x, y, kind) {
    if (kind === 0) { /* down */
        if (x > W - 80 && y < 72) {
            scene();
            px = -1;
            return;
        }
        ci = (ci + 1) % COLORS.length;
        ui.rect(x - 2, y - 2, 5, 5, COLORS[ci]);
        px = x;
        py = y;
    } else if (kind === 1 && px >= 0) { /* move */
        ui.line(px, py, x, y, COLORS[ci]);
        ui.line(px + 1, py, x + 1, y, COLORS[ci]); /* 2px 幅 */
        px = x;
        py = y;
    } else if (kind === 2) { /* up */
        px = -1;
    }
});
