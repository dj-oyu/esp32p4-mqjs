// @app battery_trace
// @title バッテリー計測
// @icon
// @desc 満充電から自動シャットダウンまでの (電圧, 電流) を MQTT へ記録。
// @perm mqtt
// バッテリーの実測トレース。OCV-SoC テーブルを実機データで作り直すための
// 計測アプリで、常用するものではありません。
//
// 使い方 (docs/battery-power-design.md の「較正手順」):
//   1. 満充電にする (充電上限を 100% にして、緑が消えるまで待つ)
//   2. USB を抜き、このアプリを起動して放置する (約 6 時間)
//   3. 端末が自動シャットダウンしたら、購読側のログを保存する
//        mosquitto_sub -h <broker> -t 'esp32p4-mqjs/battery' -v > trace.log
//   4. tools/battery_fit.py trace.log で OCV テーブルを出力し、
//      components/pwr_tab5/pwr_gauge.c の s_ocv[] を差し替える
//
// 出力は 1 行 1 サンプルの CSV (10 秒間隔):
//   ms,mv,ma,ocv_mv,pct,mah,mohm,state,usb,raw
//
// 画面は消えてよい (mqjs_power が勝手に暗くします)。計測は C 側の 1Hz
// サンプラが動かしているので、この アプリは 10 秒ごとにそれを読むだけです。
"use strict";
sys.setAppName("battery_trace");

var PERIOD_MS = 10000;
var TOPIC = null;
var n = 0;
var t0 = performance.now();

var HAS_UI = ui.size()[0] !== 0;
var stLine = null, stCount = null;

function row() {
    var b = power.battery();
    return [
        Math.round(performance.now() - t0),
        b.mv, b.ma, b.ocv, b.pct, b.mah, b.mohm,
        b.state, b.usb ? 1 : 0, b.raw
    ].join(",");
}

var tick = function () {
    var line = row();
    n++;
    print("[batt] " + line);
    if (TOPIC && mqtt.connected())
        mqtt.publish(TOPIC, line);
    if (stLine)
        stLine.setText(line);
    if (stCount)
        stCount.setText(n + " サンプル (" +
                        Math.round((performance.now() - t0) / 60000) + " 分)");
};

if (HAS_UI) {
    var s = ui.screen("バッテリー計測");
    s.label("満充電から自動シャットダウンまで記録します。");
    s.label("USB を抜いて放置してください。");
    stCount = s.label("0 サンプル");
    stLine = s.label("...");
    s.button("アプリ一覧へ戻る", function () { sys.open("launcher"); });
}

/* 記録の途中で電池切れシャットダウンが来る -- それがゴールなので、最後の
   1 行をここで出しておく。onStop("battery") はハンドラごとウォッチドッグに
   囲まれているので、重い処理はしないこと。 */
sys.onStop(function (reason) {
    print("[batt] stop (" + reason + ") " + row());
    if (TOPIC && mqtt.connected())
        mqtt.publish(TOPIC, "# stop " + reason + " " + row());
});

net.onReady(function (token) {
    TOPIC = net.topic("battery");
    mqtt.connect(token);
    print("[batt] publishing to " + TOPIC + " every " + PERIOD_MS + "ms");
});

tick();
setInterval(tick, PERIOD_MS);
