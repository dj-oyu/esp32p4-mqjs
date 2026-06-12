// dev-slot probe: exercise the camera.scan pipeline end to end without
// serial. Reports camera.status() before/after and the scan result
// (null = timeout, which still proves init+capture+event delivery) on
// <base>/proberep, then stops. Restore dev_idle.js afterwards.
"use strict";
sys.setAppName("probe_cam");

var BASE = "esp32p4-mqjs/task/u7q3x9f2";

function pub(o) { mqtt.publish(BASE + "/proberep", JSON.stringify(o), 0, 0); }

function selfStop() {
    var a = sys.apps();
    for (var i = 0; i < a.length; i++)
        if (a[i].name === "probe_cam") sys.stop(a[i].slot);
}

mqtt.onConnect(function () {
    var st0 = camera.status();
    var rc = camera.scan(function (code) {
        pub({ phase: "cam-done", result: code || null,
              after: camera.status() });
        setTimeout(selfStop, 1000);
    }, "97");
    pub({ phase: "cam-start", rc: rc, before: st0, after: camera.status() });
    if (!rc)
        setTimeout(selfStop, 2000);
});
mqtt.connect("mqtt://192.168.1.2");
