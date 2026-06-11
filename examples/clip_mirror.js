// @app clip_mirror
// @title クリップボード同期
// @desc クリップボードを MQTT (retained) で他デバイスと共有する常駐ブリッジ。ブローカー不在時はローカルのみ。
// @perm mqtt,clipboard
// @autostart
//
// T3b 層2: クリップボードの MQTT ミラー (ssh-terminal-design §7/§7.1)。
// 同期は API に焼き込まず、この小アプリが clipboard.onChange と
// mqtt を橋渡しする。ローカルファースト: ブローカーが落ちていれば
// 黙ってローカルのみで動き、再接続時に retained が再同期する。
//
//  - 送信: clipboard.onChange → retained publish (他アプリの set だけ
//    が onChange を起こす。自分の set は除外されるので echo ループの
//    芽が API 側で摘まれている)
//  - 受信: subscribe → from が自分なら無視 (ブローカー経由の echo)、
//    現在値と同一ならスキップ (retained 再配送で荒れない)、差分だけ
//    clipboard.set + 通知
//  - 注意: 受信ペイロード上限 4096B。4000B 級のクリップは JSON の
//    包みで超えることがあり、その同期は黙って落ちる (割り切り)
"use strict";
sys.setAppName("clip_mirror");

var BROKER = "mqtt://192.168.1.2";
var TOPIC = "esp32p4-mqjs/clipboard";

/* 送信者 ID (echo 識別)。初回に乱数生成して store に永続。 */
var MY = store.get("clipmirror_id");
if (!MY) {
    MY = "tab5-" + ((Math.random() * 0xFFFFFF) | 0).toString(16);
    store.set("clipmirror_id", MY);
}

/* 直近に publish した値。EV_CLIP はペイロードを運ばず「配送時の現在値」
   を読む (latest wins) ので、連続 set は同じ最終値を複数回見せてくる —
   同一値の再 publish はここで畳む (受信側の冪等ガードと対) */
var sentData = null, sentType = null;

clipboard.onChange(function (data, type) {
    if (!mqtt.connected())
        return; /* 日和見: ブローカー不在なら何もしない */
    if (data === sentData && type === sentType)
        return;
    sentData = data;
    sentType = type;
    mqtt.publish(TOPIC, JSON.stringify({ from: MY, type: type, data: data }),
                 0, 1 /* retain = ストアの棚と同じ「最新が残る」意味論 */);
});

mqtt.subscribe(TOPIC, function (t, payload) {
    var m;
    try {
        m = JSON.parse(payload);
    } catch (e) {
        return;
    }
    if (!m || m.from === MY || typeof m.data !== "string")
        return;
    var type = m.type || "text/plain";
    sentData = m.data; /* ブローカー既知の値: 同値の再送をここでも畳む */
    sentType = type;
    var cur = clipboard.get();
    if (cur && cur.data === m.data && cur.type === type)
        return; /* 冪等: 同一値はスキップ */
    clipboard.set(m.data, type);
    sys.notify("クリップボード受信 " + type + " (" + m.data.length + "B)");
});

mqtt.onConnect(function () {
    print("[clip_mirror] connected as " + MY);
});
mqtt.connect(BROKER);
