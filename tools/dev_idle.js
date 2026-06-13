// dev slot idle stub: one-shot install/state report over MQTT, then
// stop (auto-rerun held until the next push). Persisted on purpose -
// each boot publishes one devreport line and goes quiet.
"use strict";
sys.setAppName("dev_idle");

function stopSelf() {
    var a = sys.apps();
    for (var k = 0; k < a.length; k++)
        if (a[k].name === "dev_idle") sys.stop(a[k].slot);
}

/* net.onReady でリンク確立を待ってから接続 (token は capability、これ無しに
   mqtt.connect は呼べない)。broker はプラットフォーム既定 = mqtt.connect(token)。
   devreport は dev のタスクチャンネル宛なので固定トピックに publish。
   接続はリンク確立(=数秒〜十数秒)後なので、固定タイマーで先に止めず、
   publish 後に自停止する (リンクが来なければ fallback で静かに止まる)。 */
net.onReady(function (token) {
    mqtt.onConnect(function () {
        var inst = sys.installed(), names = [];
        for (var i = 0; i < inst.length; i++) names.push(inst[i].name);
        var st = sys.store(), have = [];
        for (var j = 0; j < st.length; j++)
            if (st[j].installed) have.push(st[j].name);
        mqtt.publish("esp32p4-mqjs/task/u7q3x9f2/devreport",
                     JSON.stringify({ installed: names, storeInstalled: have }),
                     0, 0);
        setTimeout(stopSelf, 500);   /* report sent: go quiet */
    });
    mqtt.connect(token);
});

setTimeout(stopSelf, 30000);   /* fallback: link never came up */
