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

/* 2 連続スキャンで「2 回目の STREAMON 失敗」リグレッションも検出する。
 * 1 回目はすぐ cancel して短縮、2 回目はフルタイムアウトまで回す */
mqtt.onConnect(function () {
    var st0 = camera.status();
    var rc1 = camera.scan(function (c1) {
        pub({ phase: "scan1-done", result: c1 || null,
              after: camera.status() });
        var rc2 = camera.scan(function (c2) {
            pub({ phase: "scan2-done", result: c2 || null,
                  after: camera.status() });
            setTimeout(selfStop, 1000);
        }, "97");
        pub({ phase: "scan2-start", rc: rc2, after: camera.status() });
        if (!rc2)
            setTimeout(selfStop, 2000);
    }, "97");
    pub({ phase: "scan1-start", rc: rc1, before: st0,
          after: camera.status() });
    if (!rc1) {
        setTimeout(selfStop, 2000);
        return;
    }
    setTimeout(function () { camera.cancel(); }, 6000);
});
net.onReady(function (token) { mqtt.connect(token); });
