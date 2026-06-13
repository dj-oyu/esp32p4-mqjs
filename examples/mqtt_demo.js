// @app mqtt_demo
// @title MQTTデモ
// @icon 
// @desc 公開ブローカーへのループバック送受信デモ。
// @perm mqtt
// MQTT デモ: パブリックブローカーに接続し、自分の publish を自分の
// subscribe で受けるループバック。配線不要 (WiFi 必須)。
//
// コア (接続 + 3 秒ごとの自動 publish) はヘッドレスでも従来どおり動く。
// 画面あり (Tab5) ならウィジェットのコントロールパネルを重ねる:
// 状態・最終受信の表示 (setText)、自動 publish のトグル、即時 publish。
//
// JS API:
//   mqtt.connect(uri)                  - "mqtt://host[:port]" で接続開始
//   mqtt.onConnect(fn)                 - ブローカー接続確立ごとに fn()
//   mqtt.subscribe(topic, fn)          - fn(topic, payload)。+ / # 可
//   mqtt.publish(topic, payload[, qos, retain])
//   mqtt.connected()                   - 1/0
//   mqtt.disconnect()                  - セッション破棄 (ループ終了要因)
"use strict";
sys.setAppName("mqtt_demo");

var HAS_UI = ui.size()[0] !== 0;

var BROKER = "mqtt://test.mosquitto.org";
var TOPIC = "esp32p4-mqjs/demo";
var n = 0;
var autoPub = true;

/* ウィジェットは作った後で setText する (画面なし機ではダミーの no-op) */
var stConn = null, stRx = null;
var lastRx = "";

mqtt.onConnect(function () {
    print("[mqtt_demo] connected to " + BROKER);
    if (stConn)
        stConn.setText("接続中: " + BROKER);
});

// 接続前に subscribe してよい (接続確立時にまとめて購読される)
mqtt.subscribe(TOPIC, function (t, p) {
    print("[mqtt_demo] rx " + t + " = " + p);
    lastRx = p;
    if (stRx)
        stRx.setText("rx: " + p);
});

/* net.onReady でリンク確立(=token)を待ってから接続。token は capability で、
   これ無しに mqtt.connect は呼べない。BROKER は公開テストブローカーなので明示
   指定する (省略すればプラットフォーム既定 broker = mqtt.connect(token))。 */
net.onReady(function (token) {
    mqtt.connect(token, BROKER);
});

function publishNow() {
    if (!mqtt.connected())
        return;
    n++;
    mqtt.publish(TOPIC, "hello from mqjs #" + n);
}

setInterval(function () {
    if (autoPub)
        publishNow();
}, 3000);

/* P4b: 画面は切替時に破棄されるので、初回もフォアグラウンド復帰も
   同じ buildPanel で現在のモデルから作り直す */
var buildPanel = function () {
    var s = ui.screen("MQTT デモ");
    stConn = s.label((mqtt.connected() ? "接続中: " : "接続待ち: ") + BROKER);
    s.label("topic: " + TOPIC);
    stRx = s.label("rx: " + (lastRx || "(まだ)"));
    s.toggle("3 秒ごとに自動 publish", autoPub ? 1 : 0,
             function (v) { autoPub = v !== 0; });
    s.button("今すぐ publish", function () { publishNow(); });
    s.button("コンソールへ戻る", ui.back);
};

if (HAS_UI) {
    buildPanel();
    sys.onForeground(buildPanel);
}
