// dev-slot probe: run exactly ONE barcode scan (45 s timeout, point at
// nothing) and report the L2 view before/after over MQTT. Companion to
// the periodic heap dump: the dump before the scan vs the dump after
// names the long-lived blocks a first camera scan pins into the heap.
// Restore dev_idle.js afterwards.
"use strict";
sys.setAppName("probe_scan1");

var BASE = "esp32p4-mqjs/task/u7q3x9f2";

function pub(o) { mqtt.publish(BASE + "/proberep", JSON.stringify(o), 0, 0); }

function selfStop() {
    var a = sys.apps();
    for (var i = 0; i < a.length; i++)
        if (a[i].name === "probe_scan1") sys.stop(a[i].slot);
}

function snap() {
    var h = sys.heap();
    return { l2: h[3], max: h[4], lo: h[5] };
}

mqtt.onConnect(function () {
    pub({ before: snap() });
    var ok = camera.scan(function (code) {
        pub({ scanned: code ? code : null, after: snap() });
        /* settle a bit so the post-scan periodic dump sees steady state */
        setTimeout(function () {
            pub({ settled: snap() });
            setTimeout(selfStop, 500);
        }, 20000);
    });
    if (!ok) { pub({ err: "scan refused" }); setTimeout(selfStop, 500); }
});

net.onReady(function (token) { mqtt.connect(token); });
