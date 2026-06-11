// P4a 実機検証プローブ (dev スロットに push する):
//   - dev スロット置換 (push -> 即切替) が多アプリ化後も動くこと
//   - dev と bg_app が「それぞれ自分の」mqtt クライアントを同時に持てること
//     (test.mosquitto.org の p4a-dev と p4a-bg が同時に流れる)
//   - sys.signal("bg_app", ...) の名前解決と投函 (sig=1 が publish に乗る。
//     受信側の print はデバイスのコンソールに [bg_app] 行で出る)
"use strict";

var n = 0;
var sigOk = -1;

mqtt.onConnect(function () { print("dev probe mqtt up"); });
mqtt.connect("mqtt://test.mosquitto.org");

setInterval(function () {
    n++;
    if (n <= 3)
        sigOk = sys.signal("bg_app", "ping#" + n) ? 1 : 0;
    if (mqtt.connected())
        mqtt.publish("esp32p4-mqjs/p4a-dev", "dev alive #" + n + " sig=" + sigOk);
}, 3000);

print("dev probe started");
