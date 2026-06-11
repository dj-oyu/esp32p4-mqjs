// w2_unwind_probe.js — s_cur 修正の実機自動検証 (画面操作なし)。
// 3 画面積んで同一 tick 内で全部 unwind する (アニメ完了を待たない =
// 競合していたまさにそのパターン)。修正前は 1〜2 pop で止まり、
// 修正後は必ず 3 pop になる。結果は MQTT で報告。
"use strict";

var r1 = [];
ui.screen("probe-1");
ui.screen("probe-2");
ui.screen("probe-3");
var pops = 0;
while (ui.back())
    pops++;
r1.push("burst_pops=" + pops + (pops === 3 ? " OK" : " FAIL"));

// もう 1 周: 直後に screen() を作るパターン (削除→一覧再構築の形)
ui.screen("probe-4");
ui.screen("probe-5");
var pops2 = 0;
while (ui.back())
    pops2++;
ui.screen("probe-6");
var pops3 = 0;
while (ui.back())
    pops3++;
r1.push("rebuild_pops=" + pops2 + "+" + pops3 +
        ((pops2 === 2 && pops3 === 1) ? " OK" : " FAIL"));

var h = sys.heap();
r1.push("lvgl=" + h[2]);

mqtt.onConnect(function () {
    mqtt.publish("esp32p4-mqjs/w2test", r1.join(" "));
    setTimeout(function () { mqtt.disconnect(); }, 1500);
});
mqtt.connect("mqtt://192.168.1.2");
