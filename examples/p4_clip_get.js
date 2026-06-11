// P4d reboot-persistence half: reads the clipboard WITHOUT setting it
// and publishes the result. Push after p4_clip_probe.js, then reboot
// the Tab5 — this persisted task re-runs at boot, so its publish shows
// whether "number|123" survived the power cycle via NVS (§7.1).
"use strict";
sys.setAppName("clip_get");

var g = clipboard.get();
var r = "boot-get=" + (g ? g.type + "|" + g.data : "undefined");

mqtt.onConnect(function () {
    mqtt.publish("esp32p4-mqjs/clipprobe", r);
    print("[clip_get] " + r);
    setTimeout(function () { mqtt.disconnect(); }, 1500);
});
mqtt.connect("mqtt://192.168.1.2");
