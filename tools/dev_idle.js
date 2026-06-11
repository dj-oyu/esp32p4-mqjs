// dev slot idle stub: one-shot install/state report over MQTT, then
// stop (auto-rerun held until the next push). Persisted on purpose -
// each boot publishes one devreport line and goes quiet.
"use strict";
sys.setAppName("dev_idle");
mqtt.onConnect(function () {
    var inst = sys.installed(), names = [];
    for (var i = 0; i < inst.length; i++) names.push(inst[i].name);
    var st = sys.store(), have = [];
    for (var j = 0; j < st.length; j++)
        if (st[j].installed) have.push(st[j].name);
    mqtt.publish("esp32p4-mqjs/task/u7q3x9f2/devreport",
                 JSON.stringify({ installed: names, storeInstalled: have }),
                 0, 0);
});
mqtt.connect("mqtt://192.168.1.2");
setTimeout(function () {
    var a = sys.apps();
    for (var k = 0; k < a.length; k++)
        if (a[k].name === "dev_idle") sys.stop(a[k].slot);
}, 6000);
