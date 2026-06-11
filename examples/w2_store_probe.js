// w2_store_probe.js — W2 store.* の実機自動検証 (画面操作なし)。
// 前回実行時に書いた値を読み戻して MQTT で報告する。タスクは終了後
// 自動再実行されるので同一ブート内の永続が、ウォッチドッグ再起動を
// 挟めばリブート跨ぎの永続が、購読側からそのまま観測できる。
"use strict";

var prev = store.get("w2test");
var hosts = store.get("ssh_hosts");
var stamp = "t" + Date.now();
var ok = store.set("w2test", stamp);

mqtt.onConnect(function () {
    mqtt.publish("esp32p4-mqjs/w2test",
                 "prev=" + prev + " now=" + stamp + " setok=" + ok +
                 " hosts=" + (hosts === undefined ? "(none)" : hosts));
    setTimeout(function () { mqtt.disconnect(); }, 1500);
});
mqtt.connect("mqtt://192.168.1.2");
