// MQTT デモ: パブリックブローカーに接続し、自分の publish を
// 自分の subscribe で受けるループバック。配線不要 (WiFi 必須)。
//
// JS API:
//   mqtt.connect(uri)                  - "mqtt://host[:port]" で接続開始
//   mqtt.onConnect(fn)                 - ブローカー接続確立ごとに fn()
//   mqtt.subscribe(topic, fn)          - fn(topic, payload)。+ / # 可
//   mqtt.publish(topic, payload[, qos, retain])
//   mqtt.connected()                   - 1/0
//   mqtt.disconnect()                  - セッション破棄 (ループ終了要因)

var BROKER = "mqtt://test.mosquitto.org";
var TOPIC = "esp32p4-mqjs/demo";
var n = 0;

mqtt.onConnect(function () {
    print("[mqtt_demo] connected to " + BROKER);
});

// 接続前に subscribe してよい (接続確立時にまとめて購読される)
mqtt.subscribe(TOPIC, function (t, p) {
    print("[mqtt_demo] rx " + t + " = " + p);
});

mqtt.connect(BROKER);

setInterval(function () {
    if (!mqtt.connected())
        return;
    n++;
    mqtt.publish(TOPIC, "hello from Stamp-P4 #" + n);
}, 3000);
