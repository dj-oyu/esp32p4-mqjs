// dev-slot probe: time a burst of 20 store.set calls. With the old
// synchronous nvs_commit this took tens-to-hundreds of ms (one flash
// commit per call, blocking every app); with write-behind it should be
// single-digit ms (RAM cache writes + one deferred commit).
"use strict";
sys.setAppName("probe_store_t");

var BASE = "esp32p4-mqjs/task/u7q3x9f2";

mqtt.onConnect(function () {
    var t0 = Date.now();
    for (var i = 0; i < 20; i++)
        store.set("perf_k", "v" + i +
                  "-0123456789012345678901234567890123456789");
    var dt = Date.now() - t0;
    store.del("perf_k");
    mqtt.publish(BASE + "/proberep",
                 JSON.stringify({ phase: "store20", ms: dt }), 0, 0);
    setTimeout(function () {
        var a = sys.apps();
        for (var k = 0; k < a.length; k++)
            if (a[k].name === "probe_store_t") sys.stop(a[k].slot);
    }, 1500);
});
net.onReady(function (token) { mqtt.connect(token); });
