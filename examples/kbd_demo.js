// @app kbd_demo
// @title キーボードデモ
// @icon 
// @desc オンスクリーンキーボードと履歴付き行エディタ。
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
var KB_H = 400; /* ui.keyboard(1) が覆う高さ (px)。実値は relayout() が問い合わせる */
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

/* 負数 = 純クエリ (表示は変えない)。mode 1 では 0 が「ドックが付いているので
   何も覆わない」という正解なので v>0 では弾けない — まだ答えられない
   (キャンバス未生成) 場合だけを画面サイズで見分ける。 */
function currentKb() {
    return ui.size()[0] ? ui.keyboard(-1) : KB_H;
}

/* 回転すると幅も高さもキーボードの占有高も同時に変わる (ドック接続 = 横向き
   かつオンスクリーンキーボード無し)。起動時の寸法のまま描き続けると入力行が
   画面の真ん中に取り残される。C 側は回転後キャンバスを隠すので、採り直したら
   必ず描き直すこと。 */
function relayout() {
    var s = ui.size();
    W = s[0] || W; /* [0,0] = 画面なし: 前の値を保つ */
    H = s[1] || H;
    KB_H = currentKb();
    VIEW_H = H - KB_H;
    INPUT_Y = VIEW_H - LH - 8;
    MAX_HIST = ((INPUT_Y - 40) / LH) | 0;
    if (MAX_HIST < 1)
        MAX_HIST = 1;
    while (hist.length > MAX_HIST)
        hist.shift();
    redraw();
}

ui.keyboard(1);
relayout();

/* トークンは前面アプリにしか届かない。取りこぼした一回でズレっぱなしになるので
   副作用の無いクエリで突き合わせる。 */
setInterval(function () {
    var s = ui.size();
    if (s[0] !== W || s[1] !== H || currentKb() !== KB_H)
        relayout();
}, 500);

/* 背面では描画コマンドが捨てられる — 上のポーリングだけだと、裏で回転を拾って
   寸法だけ新しくなり、前面に戻っても一致するので二度と描き直さない */
sys.onForeground(function () {
    ui.keyboard(1);
    relayout();
});

ui.onKey(function (k) {
    /* 回転は下の分岐より前に捌く — 素通りすると末尾の else が編集行に NUL を混ぜる */
    if (k.charCodeAt(0) === 0 && k.slice(1) === "rotate") {
        relayout();
        return;
    }
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
