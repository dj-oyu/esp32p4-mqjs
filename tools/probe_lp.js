// dev-slot probe: LP SRAM retention (docs/term-design.md §11.3, the
// prerequisite of the §4.4 black-box ring).
//
// Two-visit protocol, because the device crashes itself in between:
//
//   1st push  -> nothing collected yet, so ARM the sequence and stop.
//                The C sequencer then runs on its own: every boot it
//                judges the RTC_NOINIT region, records a verdict, and
//                15 s later triggers the next reset cause (panic, task
//                WDT, int WDT, esp_restart). Four resets, ~1-2 minutes.
//   2nd push  -> results are in NVS, so PUBLISH them and stop.
//
// Re-pushing in between is harmless: arming is one-shot on the C side
// (a no-op while a sequence runs or while results exist), and this
// script only ever reports what it finds. Which matters, because the
// dev slot re-runs its script on every boot — including the four boots
// the sequence itself causes.
//
// To run it again: sys.lpProbe("clear") wipes state + verdicts.
// "clear" during a pre-crash delay window also aborts the sequence.
//
// Reports on <base>/proberep. Restore dev_idle.js afterwards.
"use strict";
sys.setAppName("probe_lp");

var BASE = "esp32p4-mqjs/task/u7q3x9f2";

function pub(o) { mqtt.publish(BASE + "/proberep", JSON.stringify(o), 0, 0); }

mqtt.onConnect(function () {
    // The C side owns the JSON; parse it only to branch, and forward the
    // parsed object so a field added there needs no change here.
    var rep = null;
    try {
        rep = JSON.parse(sys.lpProbe());
    } catch (e) {
        pub({ phase: "lp", error: "unparsable report" });
        setTimeout(function () { sys.stop("probe_lp"); }, 500);
        return;
    }

    var done = rep.results && rep.results.length >= rep.causes.length;
    if (done) {
        pub({ phase: "lp", stage: "results", rep: rep });
    } else if (rep.state === "idle") {
        var armed = JSON.parse(sys.lpProbe("arm"));
        pub({ phase: "lp", stage: "armed", state: armed.state,
              region: armed.region, addr: armed.addr,
              delay_s: armed.delay_s, causes: armed.causes,
              note: "device will now crash-cycle; re-push to collect" });
    } else {
        // mid-sequence: this boot is one of the four the probe caused.
        pub({ phase: "lp", stage: "running", state: rep.state,
              step: rep.step, pend: rep.pend,
              have: rep.results ? rep.results.length : 0,
              want: rep.causes.length });
    }
    setTimeout(function () { sys.stop("probe_lp"); }, 500);
});

net.onReady(function (token) { mqtt.connect(token); });
