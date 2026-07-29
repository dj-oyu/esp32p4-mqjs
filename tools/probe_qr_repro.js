// dev-slot probe: reproduce the QR scan-end callback hang seen 2026-07-29.
// Two runs, watched from serial (tools/capture_com8.py, passive attach)
// and MQTT at once:
//   ctrl : camera.scanQr ALONE            -> expect cb(null) at ~45 s
//   chain: camera.scan, and from inside its completion callback start
//          camera.scanQr (the original repro shape) -> the hang, if it
//          reproduces, is a missing "qr2-cb" line
// A 5 s heartbeat publishes phase + L2 free the whole time, so a silent
// stretch >50 s after "qr*-start" with heartbeats still flowing = C side
// stuck, JS alive — same signature as the original event.
// Point the camera at nothing. Restore dev_idle.js afterwards.
"use strict";
sys.setAppName("probe_qrr");

var BASE = "esp32p4-mqjs/task/u7q3x9f2";

function pub(o) { mqtt.publish(BASE + "/proberep", JSON.stringify(o), 0, 0); }

function selfStop() {
    var a = sys.apps();
    for (var i = 0; i < a.length; i++)
        if (a[i].name === "probe_qrr") sys.stop(a[i].slot);
}

var phase = "boot";
var t0 = 0;

function now() { return sys.micros(); }
function dt() { return (now() - t0) / 1000; } /* ms */

function chainRun() {
    phase = "chain-scan";
    t0 = now();
    pub({ mark: "chain-scan-start" });
    var ok = camera.scan(function (code) {
        pub({ mark: "chain-scan-cb", ms: dt(), code: code ? code : null });
        phase = "chain-qr";
        t0 = now();
        pub({ mark: "qr2-start" });
        /* scanQr is a SYSTEM API (provisioning payloads may carry WiFi
           secrets) — from the dev slot it THROWS. Uncaught, that throw
           silently kills this callback's continuation: exactly the
           2026-07-29 "hang". Catch it and the state machine survives. */
        var ok2 = false, err2 = null;
        try {
            ok2 = camera.scanQr(function (c2) {
                pub({ mark: "qr2-cb", ms: dt(), got: c2 ? "code" : null });
                pub({ verdict: "both QR runs completed" });
                setTimeout(selfStop, 1000);
            });
        } catch (e) { err2 = String(e); }
        if (err2) {
            pub({ mark: "qr2-threw", err: err2 });
            pub({ verdict: "policy throw caught; probe state machine intact" });
            setTimeout(selfStop, 1000);
        } else if (!ok2) {
            pub({ mark: "qr2-refused" });
            setTimeout(selfStop, 1000);
        }
    });
    if (!ok) { pub({ mark: "chain-scan-refused" }); setTimeout(selfStop, 1000); }
}

function ctrlRun() {
    phase = "ctrl-qr";
    t0 = now();
    pub({ mark: "qr1-start" });
    var ok = false, err = null;
    try {
        ok = camera.scanQr(function (code) {
            pub({ mark: "qr1-cb", ms: dt(), got: code ? "code" : null });
            setTimeout(chainRun, 3000);
        });
    } catch (e) { err = String(e); }
    if (err) { pub({ mark: "qr1-threw", err: err }); setTimeout(chainRun, 3000); }
    else if (!ok) { pub({ mark: "qr1-refused" }); setTimeout(chainRun, 3000); }
}

mqtt.onConnect(function () {
    setInterval(function () {
        var h = sys.heap();
        pub({ hb: phase, ms: dt(), l2: h[3], max: h[4] });
    }, 5000);
    setTimeout(ctrlRun, 3000);
});

net.onReady(function (token) { mqtt.connect(token); });
