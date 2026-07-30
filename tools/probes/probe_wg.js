// dev-slot probe: WireGuard-path load test against a tailnet host.
// Target is level-infinity's SSH port (100.124.214.100:22, confirmed
// open): http.get "fails", but the TCP connect, the server's SSH
// banner, and the teardown all cross the WG tunnel — every fetch is a
// real encrypt/decrypt round trip. Reports per-fetch wall time + the
// L2 view (sys.heap [3..5]) before/during/after, so the output shows
// what WG traffic costs in internal SRAM and whether it is returned.
// Restore dev_idle.js afterwards.
"use strict";
sys.setAppName("probe_wg");

var BASE = "esp32p4-mqjs/task/u7q3x9f2";
var WG_URL = "http://100.124.214.100:22/"; // tailnet linux box, SSH open
var FETCHES = 8;

function pub(o) { mqtt.publish(BASE + "/proberep", JSON.stringify(o), 0, 0); }

function selfStop() {
    var a = sys.apps();
    for (var i = 0; i < a.length; i++)
        if (a[i].name === "probe_wg") sys.stop(a[i].slot);
}

function snap() {
    var h = sys.heap();
    return { l2: h[3], max: h[4], lo: h[5] };
}

var minL2 = 1 << 30, minMax = 1 << 30;

function track() {
    var h = sys.heap();
    if (h[3] < minL2) minL2 = h[3];
    if (h[4] < minMax) minMax = h[4];
}

function fetchLoop(n, t0all) {
    if (n <= 0) {
        var end = snap();
        pub({ done: true, fetches: FETCHES,
              total_ms: (sys.micros() - t0all) / 1000,
              l2_min: minL2, max_min: minMax, end: end });
        setTimeout(selfStop, 1000);
        return;
    }
    var t0 = sys.micros();
    var rc = http.get(WG_URL, function (body, st) {
        track();
        pub({ fetch: FETCHES - n + 1, st: st,
              len: body ? body.length : 0,
              ms: (sys.micros() - t0) / 1000 });
        fetchLoop(n - 1, t0all);
    });
    if (!rc) {
        pub({ fetch: FETCHES - n + 1, err: "get refused" });
        fetchLoop(n - 1, t0all);
    }
}

mqtt.onConnect(function () {
    pub({ start: true, target: WG_URL, base: snap() });
    setInterval(track, 1000);
    fetchLoop(FETCHES, sys.micros());
});

net.onReady(function (token) { mqtt.connect(token); });
