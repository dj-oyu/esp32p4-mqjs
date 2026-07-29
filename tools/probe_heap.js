// dev-slot probe: free heap headroom before flashing term_core.
// sys.heap() = [internal free, PSRAM free, lvgl free] in bytes.
// term_core's console term budget is ~110KB PSRAM (lazy, on first
// term.* use) + a few hundred bytes of .bss — this says whether the
// running firmware has that headroom. Reports on <base>/proberep,
// then stops. Restore dev_idle.js afterwards.
"use strict";
sys.setAppName("probe_heap");

var BASE = "esp32p4-mqjs/task/u7q3x9f2";

function pub(o) { mqtt.publish(BASE + "/proberep", JSON.stringify(o), 0, 0); }

mqtt.onConnect(function () {
    var h = sys.heap();
    pub({ phase: "heap", internal: h[0], psram: h[1], lvgl: h[2] });
    setTimeout(function () { sys.stop("probe_heap"); }, 500);
});

net.onReady(function (token) { mqtt.connect(token); });
