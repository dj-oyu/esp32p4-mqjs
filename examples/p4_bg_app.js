// @app p4_bg_app
// @title バックグラウンドデモ
// @desc 背景実行 + onForeground 再構築 + sys.signal IPC を体験できる P4a デモ。
// @icon 
// バックグラウンド実行のデモアプリ (P4a 検証から派生)。push すると
// 上のディレクティブにより /littlefs/apps/ にインストールされ、
// ランチャーから起動できる (dev タスクとしては実行されない)。
//
// 見せるもの:
//   1. 背景でも実行が続く: 1 秒ごとの tick と MQTT ループバックが
//      前面が ssh_vt でも止まらない
//   2. 前面復帰で画面を自前で再構築する (sys.onForeground)
//   3. sys.signal / sys.onSignal の最小 IPC
//      (dev タスクから sys.signal("p4_bg_app", "...") で送れる)
"use strict";
sys.setAppName("p4_bg_app");

var BROKER = "mqtt://test.mosquitto.org";
var TOPIC = "esp32p4-mqjs/p4a-bg";

var ticks = 0;
var rxCount = 0;
var lastRx = "(なし)";
var lastSig = "(なし)";
var fgCount = 0;

/* 前面のときだけ実体があるウィジェット (背景中はダミー no-op) */
var stTick = null, stRx = null, stSig = null;

setInterval(function () {
    ticks++;
    if (ticks % 10 === 0)
        print("alive: tick=" + ticks + " rx=" + rxCount);
    if (stTick)
        stTick.setText("tick: " + ticks + " (fg " + fgCount + "回目)");
}, 1000);

mqtt.onConnect(function () {
    print("mqtt connected: " + BROKER);
});
mqtt.subscribe(TOPIC, function (t, p) {
    rxCount++;
    lastRx = p;
    if (stRx)
        stRx.setText("rx#" + rxCount + ": " + p);
});
/* net.onReady でリンク確立(=token)を待ってから接続 (token は capability)。
   BROKER は公開テストブローカーなので明示指定。 */
net.onReady(function (token) {
    mqtt.connect(token, BROKER);
});

setInterval(function () {
    if (mqtt.connected())
        mqtt.publish(TOPIC, "bg loop #" + ticks);
}, 5000);

sys.onSignal(function (value, from) {
    lastSig = value + " (from " + from + ")";
    print("signal: " + lastSig);
    if (stSig)
        stSig.setText("signal: " + lastSig);
});

/* 決定事項 3: 画面は切替時に破棄されるので、復帰のたびにモデルから
   作り直す。ここが P4a の本丸。 */
sys.onForeground(function () {
    fgCount++;
    var s = ui.screen("bg_app (P4a)");
    stTick = s.label("tick: " + ticks + " (fg " + fgCount + "回目)");
    stRx = s.label("rx#" + rxCount + ": " + lastRx);
    stSig = s.label("signal: " + lastSig);
    s.button("dev タスクへ切替", function () { sys.focus("dev"); });
});

sys.onBackground(function () {
    /* 画面はこの直後に C 側で全破棄される: ダミー化しておく */
    stTick = stRx = stSig = null;
    print("background (tick=" + ticks + ")");
});

print("bg_app started: broker=" + BROKER + " topic=" + TOPIC);
