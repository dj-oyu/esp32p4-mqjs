// P4d device smoke: typed clipboard set/get roundtrip on the Tab5,
// results published to MQTT (no serial needed — opening COM8 reboots
// the box). Push as a dev task:
//   python3 tools/mqjs_push.py 192.168.1.2 <task-topic> examples/p4_clip_probe.js
// then watch:  mosquitto_sub -h 192.168.1.2 -t esp32p4-mqjs/clipprobe -C 1
// Leaves "123" (type number) on the clipboard so a following ssh_vt
// Paste tap doubles as the cross-app + NVS-persistence check.
"use strict";
sys.setAppName("clip_probe");

var r = [];
r.push("set1=" + clipboard.set("hello-tab5", "text/plain"));
var g = clipboard.get();
r.push("get1=" + (g ? g.type + "|" + g.data : "undefined"));
r.push("set2=" + clipboard.set(123, "number"));
g = clipboard.get();
r.push("get2=" + (g ? g.type + "|" + g.data : "undefined"));

mqtt.onConnect(function () {
    mqtt.publish("esp32p4-mqjs/clipprobe", r.join(" "));
    print("[clip_probe] " + r.join(" "));
    setTimeout(function () { mqtt.disconnect(); }, 1500);
});
mqtt.connect("mqtt://192.168.1.2");
