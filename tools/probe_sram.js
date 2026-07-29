// dev-slot probe: firmware-wide SRAM telemetry under real load, without
// serial. Streams the extended sys.heap() every 2 s to <base>/proberep
// and walks through load phases:
//
//   idle1 (10 s)  -> baseline with WiFi+Tailscale up
//   wg    (~15 s) -> 5x http.get to this PC's TAILNET address
//                    (100.83.214.77) — drives the WireGuard encrypt
//                    path. Start `wsl python3 tools/mqjs_webui.py
//                    --port 8799` on the PC first; failures are
//                    recorded and harmless (the traffic still crossed
//                    the tunnel).
//   scan  (<=45 s)-> camera.scan barcode (microlink suspends, scan
//                    scratch allocates). Point it at anything, or tap
//                    outside the viewfinder to end the phase early.
//   qr    (<=45 s)-> camera.scanQr, same deal.
//   idle2 (10 s)  -> did everything return to baseline?
//
// Fields per sample: l2 free / l2 largest-contiguous / l2 low-water
// since boot (MALLOC_CAP_DMA = pure L2MEM view), legacy internal free
// (includes the 32 KB LP SRAM overflow region — hence l2 is the honest
// number), LP free, PSRAM free. The summary gives per-phase minima, so
// even without reading the stream you get "camera cost X KB of L2,
// and it came back".  Restore dev_idle.js afterwards.
"use strict";
sys.setAppName("probe_sram");

var BASE = "esp32p4-mqjs/task/u7q3x9f2";
var WG_URL = "http://100.83.214.77:8799/"; // this PC over the tailnet

function pub(o) { mqtt.publish(BASE + "/proberep", JSON.stringify(o), 0, 0); }

function selfStop() {
    var a = sys.apps();
    for (var i = 0; i < a.length; i++)
        if (a[i].name === "probe_sram") sys.stop(a[i].slot);
}

var phase = "boot";
var mins = {};   // phase -> {l2, max} minima seen while the phase ran
var samples = 0;

function sample() {
    var h = sys.heap();
    var m = mins[phase];
    if (!m) mins[phase] = m = { l2: h[3], max: h[4] };
    if (h[3] < m.l2) m.l2 = h[3];
    if (h[4] < m.max) m.max = h[4];
    samples++;
    pub({ ph: phase, l2: h[3], max: h[4], lo: h[5],
          int0: h[0], lp: h[6], ps: h[1] });
}

function setPhase(p) {
    phase = p;
    pub({ mark: p });
    sample();
}

function wgFetch(n, done) {
    if (n <= 0) { done(); return; }
    var rc = http.get(WG_URL, function (body, st) {
        pub({ ph: phase, wg: st, len: body ? body.length : 0, left: n - 1 });
        wgFetch(n - 1, done);
    });
    if (!rc) { pub({ ph: phase, wg: "get-refused" }); done(); }
}

function finish() {
    setPhase("idle2");
    setTimeout(function () {
        var h = sys.heap();
        pub({ summary: mins, samples: samples, lowWater: h[5],
              end: { l2: h[3], max: h[4], lp: h[6], ps: h[1] } });
        setTimeout(selfStop, 1000);
    }, 10000);
}

function qrPhase() {
    setPhase("qr");
    var ok = camera.scanQr(function (code) {
        pub({ ph: "qr", result: code ? "decoded" : "none" });
        finish();
    });
    if (!ok) { pub({ ph: "qr", err: "scanQr refused" }); finish(); }
}

function scanPhase() {
    setPhase("scan");
    var ok = camera.scan(function (code) {
        pub({ ph: "scan", result: code ? code : "none" });
        qrPhase();
    });
    if (!ok) { pub({ ph: "scan", err: "scan refused" }); qrPhase(); }
}

mqtt.onConnect(function () {
    setPhase("idle1");
    setInterval(sample, 2000);
    setTimeout(function () {
        setPhase("wg");
        wgFetch(5, function () { scanPhase(); });
    }, 10000);
});

net.onReady(function (token) { mqtt.connect(token); });
