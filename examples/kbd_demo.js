/* Tab5 キーボードデモ (Phase 4 布石): オンスクリーンキーボード + 行エディタ。
 * ui.textSize() でカーソル座標を計算する、JS ターミナルエミュレータの種。
 * キーボードの ×/⌨ で閉じたら画面のどこかをタップすると再表示。
 * PC では keyboard/onKey はスタブ (発火しない)。 */
"use strict";
sys.setAppName("kbd_demo");

var sz = ui.size();
var W = sz[0], H = sz[1];
if (!W)
    print("ui: この機体に画面はありません");

var BG = 0x0B0E11;
var KB_H = 400; /* ui.keyboard(1) が画面下部を覆う高さ (px) */
var cell = ui.textSize("あ"); /* [全角幅, 行高] — 半角はほぼ半分 */
var LH = cell[1] + 4;
var VIEW_H = H - KB_H; /* キーボードに隠れない領域 */
var INPUT_Y = VIEW_H - LH - 8;
var MAX_HIST = ((INPUT_Y - 40) / LH) | 0;

var line = ""; /* 編集中の行 (mquickjs の文字列はコードポイント単位) */
var hist = [];

function redraw() {
    ui.rect(0, 0, W, VIEW_H, BG);
    ui.text(8, 8, "キーボードデモ: 入力して Enter で確定", 0x4FC3F7);
    for (var i = 0; i < hist.length; i++)
        ui.text(8, 40 + i * LH, hist[i], 0xC9D1D9);
    ui.text(8, INPUT_Y, "> " + line, 0xFFFFFF);
    /* カーソル: textSize で入力行の右端を求めて下線を引く */
    var tw = ui.textSize("> " + line)[0];
    ui.rect(8 + tw + 2, INPUT_Y + LH - 6, 12, 3, 0x2ECC71);
}

ui.keyboard(1);
redraw();

ui.onKey(function (k) {
    if (k === "\n") {
        if (line.length) {
            hist.push("> " + line);
            if (hist.length > MAX_HIST)
                hist.shift();
            print("入力: " + line);
            line = "";
        }
    } else if (k === "\b") {
        line = line.slice(0, -1);
    } else if (k.charCodeAt(0) === 27) {
        /* 矢印キーは "\x1b[D" / "\x1b[C" で届く (今は未使用) */
        return;
    } else {
        line += k;
    }
    redraw();
});

/* キーボードを × で閉じたあと、タップで呼び戻す */
ui.onTouch(function (x, y, kind) {
    if (kind === 0)
        ui.keyboard(1);
});
