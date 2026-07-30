// dev-slot probe: black-box ring device verification (phase 3 §2).
// Idempotent by design: every run publishes ring stats + live tail +
// lastboot tail, then plants BBMARK lines (one via print(), one via
// term.log with SGR to prove stripping). The orchestrator resets the
// device between two runs; run 2's lastboot tail must contain run 1's
// marks, and its stats must carry the retention verdict for the reset.
// Restore dev_idle.js afterwards.
"use strict";
sys.setAppName("probe_bb");

var BASE = "esp32p4-mqjs/task/u7q3x9f2";

function pub(o) { mqtt.publish(BASE + "/proberep", JSON.stringify(o), 0, 0); }

function grab(kind) {
    var r;
    try { r = sys.blackbox(kind); } catch (e) { r = "ERR:" + e; }
    if (r === null || r === undefined) return null;
    r = "" + r;
    return (r.length > 6000) ? r.slice(r.length - 6000) : r;
}

mqtt.onConnect(function () {
    var st;
    try { st = sys.blackbox(); } catch (e) { st = "ERR:" + e; }
    pub({ phase: "bb-stats", stats: st });
    pub({ phase: "bb-lastboot", tail: grab("lastboot") });
    pub({ phase: "bb-live-pre", tail: grab("live") });

    print("BBMARK-print-77 the quick brown fox");
    var id = term.create({ name: "bb0", mode: "log" });
    if (id > 0) {
        term.log(id, "BBMARK-termlog-77 \x1b[31mRED\x1b[0m plain");
        term.close(id);
    }
    pub({ phase: "bb-planted", termId: id });

    setTimeout(function () {
        pub({ phase: "bb-live-post", tail: grab("live") });
        setTimeout(function () { sys.stop("probe_bb"); }, 300);
    }, 800);
});

net.onReady(function (token) { mqtt.connect(token); });
