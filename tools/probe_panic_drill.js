// dev-slot probe: black-box panic drill (phase 3 §2a device check).
// CONVERGENT against the boot re-run of the dev task: if the last reset
// was NOT a panic, arm sys.panic() (fires ~900 ms after connect, after
// the arm publish has left). If it WAS a panic, collect lastboot -- the
// sys/panic: note must be in it -- publish, and stop. Restore
// dev_idle.js promptly after collection: on a later non-panic boot this
// probe would fire the drill again.
"use strict";
sys.setAppName("probe_pd");

var BASE = "esp32p4-mqjs/task/u7q3x9f2";

function pub(o) { mqtt.publish(BASE + "/proberep", JSON.stringify(o), 0, 0); }

mqtt.onConnect(function () {
    var reset = "?";
    try { reset = "" + (JSON.parse("" + sys.blackbox()).reset || "?"); } catch (e) {}
    if (reset === "panic") {
        var tail;
        try { tail = "" + sys.blackbox("lastboot"); } catch (e2) { tail = "ERR:" + e2; }
        if (tail.length > 6000) tail = tail.slice(tail.length - 6000);
        pub({ phase: "pd-collect", reset: reset, tail: tail });
        setTimeout(function () { sys.stop("probe_pd"); }, 300);
    } else {
        pub({ phase: "pd-arm", reset: reset, note: "sys.panic() in ~900ms" });
        setTimeout(function () { sys.panic(); }, 400);
    }
});

net.onReady(function (token) { mqtt.connect(token); });
