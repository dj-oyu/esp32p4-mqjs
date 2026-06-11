// p4_rebuild_probe.js — P4 設計判断「背景アプリの画面は破棄して
// onForeground で再構築」の再構築コストを実測する (タスクスイッチ機構
// なし、W1-W3 プリミティブのみ)。結果は MQTT で報告。
//
// 計測 A: settings 級ウィジェットページ (≈16 widgets, リスト 6 行) の
//   構築 / ui.back() 破棄を 10 周。lvgl_port_lock 競合込みの実時間。
// 計測 B: 80x32 キャンバス全再描画の発行コスト (行あたり 1 / 4 ラン)。
//   ※ JS 側の発行時間。実ピクセルは Core1 が ~16ms 周期で消化する。
"use strict";
sys.setAppName("p4_rebuild_probe");

function pageBuild(n) {
    var s = ui.screen("Rebuild probe #" + n);
    s.label("ウィジェット再構築コスト計測");
    var st = s.label("status");
    var f1 = s.field("Host");
    var f2 = s.field("User");
    var f3 = s.field("Password", { secret: true });
    s.toggle("WiFi", 1, function () {});
    s.slider(0, 100, 50, function () {});
    var l = s.list();
    for (var i = 0; i < 6; i++)
        l.add("item-" + i, function () {});
    s.button("OK", function () {});
    s.button("Cancel", ui.back);
    return s;
}

var results = [];

/* ---- A: ウィジェットページ build / destroy ---- */
var N = 10;
var bMin = 1e9, bMax = 0, bSum = 0;
var dMin = 1e9, dMax = 0, dSum = 0;
for (var k = 0; k < N; k++) {
    delay(400); /* 直前の遷移アニメ(200ms)を済ませてから測る */
    var t0 = performance.now();
    pageBuild(k);
    var t1 = performance.now();
    ui.back();
    var t2 = performance.now();
    var b = t1 - t0, d = t2 - t1;
    bSum += b; dSum += d;
    if (b < bMin) bMin = b;
    if (b > bMax) bMax = b;
    if (d < dMin) dMin = d;
    if (d > dMax) dMax = d;
}
results.push("widget_build_clean avg=" + (bSum / N).toFixed(1) + "ms min=" +
             bMin + " max=" + bMax);
results.push("widget_destroy avg=" + (dSum / N).toFixed(1) + "ms min=" +
             dMin + " max=" + dMax);

/* ---- B: キャンバス全再描画の発行 (80x32 相当) ---- */
var cs = ui.cellSize();
var CW2 = cs[0] || 9, LH2 = cs[1] || 24;
var COLS = ((ui.size()[0] || 720) / CW2) | 0;
var ROWS = (((ui.size()[1] || 1192) - 400) / LH2) | 0;
var line = "x".repeat(COLS);
var q = line.length >> 2;
var seg = "y".repeat(q);

function fullRedraw(runsPerRow) {
    var t0 = performance.now();
    for (var r = 0; r < ROWS; r++) {
        if (runsPerRow === 1) {
            ui.cells(0, r, line, 0xC9D1D9, 0x0B0E11);
        } else { /* 4 runs */
            ui.cells(0, r, seg, 0xE05A4E, 0x0B0E11);
            ui.cells(q, r, seg, 0x2ECC71, 0x0B0E11);
            ui.cells(q * 2, r, seg, 0x4FC3F7, 0x0B0E11);
            ui.cells(q * 3, r, seg, 0xFFD479, 0x0B0E11);
        }
        if ((r & 7) === 7)
            delay(20); /* キュー(128)を溢れさせない: 8 行ごとに一息 */
    }
    return performance.now() - t0;
}

ui.clear(0x0B0E11);
results.push("canvas_1run " + fullRedraw(1).toFixed(1) + "ms (" + ROWS +
             " rows, queue-paced)");
results.push("canvas_4run " + fullRedraw(4).toFixed(1) + "ms");

var h = sys.heap();
results.push("lvgl=" + h[2]);
print(results.join(" | "));

mqtt.onConnect(function () {
    mqtt.publish("esp32p4-mqjs/w2test", results.join(" | "));
    setTimeout(function () { mqtt.disconnect(); }, 1500);
});
mqtt.connect("mqtt://192.168.1.2");
