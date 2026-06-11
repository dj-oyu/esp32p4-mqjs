/* Tab5 ui.* デモ: アナログ時計 + サイン波。ハイブリッド UI のサンプル。
 *
 *  - 描画 = キャンバスモード (ui.line/rect/text)。毎秒の針更新と 50ms の
 *    波形掃引はコマンドキュー直行の性能バイパス。
 *  - 設定 (波形の速さ/一時停止) = ウィジェットモード。右上の □ をタップ。
 *
 * キューは満杯時に非ブロッキングで捨てる設計なので、起動時の静的シーン
 * (文字盤 60 本) は delay() で小分けに流す。
 * PC (run_pc) ではスタブが print されるだけ。Stamp では全部 no-op。 */
"use strict";
sys.setAppName("ui_demo");

var sz = ui.size();
var W = sz[0], H = sz[1];
if (!W)
    print("ui: この機体に画面はありません (描画は no-op)");

var BG = 0x102030, FG = 0xE0E6EA, DIM = 0x47566A;
var ACC = 0x4FC3F7, RED = 0xE05A4E;

ui.clear(BG);
ui.text(20, 14, "mqjs ui デモ", FG);
ui.rect(W - 72, 8, 56, 56, 0x2E6BD6); /* 設定を開くボタン */

/* ---- アナログ時計: 文字盤 ---- */
var CX = Math.round(W / 2), CY = 360, R = 210;
var i;
for (i = 0; i < 60; i++) {
    var a = i * Math.PI / 30;
    var major = (i % 5 === 0);
    var r0 = major ? R - 16 : R - 7;
    ui.line(CX + Math.round(r0 * Math.sin(a)), CY - Math.round(r0 * Math.cos(a)),
            CX + Math.round(R * Math.sin(a)), CY - Math.round(R * Math.cos(a)),
            major ? FG : DIM);
    if (i % 15 === 0)
        delay(5); /* let the UI drain the queue */
}

/* ---- 針 (SNTP 未同期なら 1970 起点で動くだけ) ---- */
var hands = [null, null, null];
function two(n) { return (n < 10 ? "0" : "") + n; }

function drawClock() {
    var t = Date.now() + 9 * 3600 * 1000; /* JST */
    var sec = Math.floor(t / 1000) % 60;
    var min = Math.floor(t / 60000) % 60;
    var hr = Math.floor(t / 3600000) % 24;
    var frac = [((hr % 12) + min / 60) / 12, (min + sec / 60) / 60, sec / 60];
    var len = [R * 0.5, R * 0.72, R * 0.9];
    var col = [FG, ACC, RED];
    var k;
    for (k = 0; k < 3; k++)
        if (hands[k])
            ui.line(CX, CY, hands[k][0], hands[k][1], BG);
    for (k = 0; k < 3; k++) {
        var a = frac[k] * 2 * Math.PI;
        var x = CX + Math.round(len[k] * Math.sin(a));
        var y = CY - Math.round(len[k] * Math.cos(a));
        ui.line(CX, CY, x, y, col[k]);
        hands[k] = [x, y];
    }
    ui.rect(CX - 80, CY + R + 24, 160, 26, BG);
    ui.text(CX - 52, CY + R + 24, two(hr) + ":" + two(min) + ":" + two(sec), FG);
}
setInterval(drawClock, 1000);
drawClock();

/* ---- ライブグラフ: サイン波の掃引 (ホットパス) ---- */
ui.text(20, 860, "サイン波 (50ms 周期) — 右上 □ で設定", DIM);
var GC = 1020, GA = 100, gx = 0, ph = 0;
var waveStep = 2;      /* 掃引の速さ (px/tick): 設定スライダーで変更 */
var waveRun = true;    /* 一時停止トグル */
ui.line(0, GC, W - 1, GC, DIM);
setInterval(function () {
    if (!waveRun)
        return;
    ui.line(gx, GC - GA - 6, gx, GC + GA + 6, BG);   /* 自分の列を消す */
    var v = Math.sin(gx / 40 + ph);
    var y = GC - Math.round(v * GA);
    ui.rect(gx, y - 1, 2, 3, ACC);
    ui.pixel(gx, GC, DIM);                            /* 軸を復元 */
    gx += waveStep;
    if (gx >= W) {
        gx = 0;
        ph += 0.7;
    }
}, 50);

/* ---- ウィジェット設定画面 (ハイブリッドの「設定だけ標準 UI」部) ---- */
var inSettings = false;

function openSettings() {
    inSettings = true;
    var s = ui.screen("デモ設定");
    s.label("サイン波");
    s.toggle("掃引する", waveRun ? 1 : 0, function (v) { waveRun = v !== 0; });
    s.slider(1, 10, waveStep, function (v) { waveStep = v; });
    s.button("閉じる", function () {
        inSettings = false;
        ui.back();
    });
}

ui.onTouch(function (x, y, kind) {
    if (inSettings)
        return;
    if (kind === 0 && x > W - 80 && y < 72)
        openSettings();
});
