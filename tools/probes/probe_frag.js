// dev-slot probe: does repeated WireGuard traffic ACCUMULATE internal
// fragmentation? Six identical rounds of [steady-point sample -> 3-fetch
// WG burst -> 87 s settle]. The settle lets TIME_WAIT PCBs drain (lwIP
// MSL) so the steady points are comparable: if `max` (largest free L2
// block) declines round over round while `l2` (free) holds, the
// fragmentation-accumulation hypothesis is confirmed; if round 2..6
// steady points sit flat, the burst cost is transient and bounded.
// ~15 min total. Restore dev_idle.js afterwards.
"use strict";
sys.setAppName("probe_frag");

var BASE = "esp32p4-mqjs/task/u7q3x9f2";
var WG_URL = "http://100.124.214.100:22/";
var ROUNDS = 6;
var FETCHES = 3;
var SETTLE_MS = 87000;

function pub(o) { mqtt.publish(BASE + "/proberep", JSON.stringify(o), 0, 0); }

function selfStop() {
    var a = sys.apps();
    for (var i = 0; i < a.length; i++)
        if (a[i].name === "probe_frag") sys.stop(a[i].slot);
}

function steady(round) {
    var h = sys.heap();
    pub({ steady: round, l2: h[3], max: h[4], lo: h[5] });
}

function burst(n, doneCb) {
    if (n <= 0) { doneCb(); return; }
    var rc = http.get(WG_URL, function (body, st) {
        burst(n - 1, doneCb);
    });
    if (!rc) burst(n - 1, doneCb);
}

function round(r) {
    if (r > ROUNDS) {
        var h = sys.heap();
        pub({ done: true, rounds: ROUNDS, end_l2: h[3], end_max: h[4] });
        setTimeout(selfStop, 1000);
        return;
    }
    steady(r); /* comparable point: after the previous round's settle */
    pub({ burst: r });
    burst(FETCHES, function () {
        setTimeout(function () { round(r + 1); }, SETTLE_MS);
    });
}

mqtt.onConnect(function () {
    setTimeout(function () { round(1); }, 3000);
});

net.onReady(function (token) { mqtt.connect(token); });
