// dev-slot probe: recording mode on hardware, including the one piece
// with real risk — the panic-path screen capture (it reads the PSRAM
// grid with no lock in panic context; a failure shows up as a double
// panic). CONVERGENT against the boot re-run: it only arms the panic
// half when the last reset was NOT a panic, and on a panic boot it just
// reports what survived. Restore dev_idle.js afterwards.
"use strict";
sys.setAppName("probe_rec");

var BASE = "esp32p4-mqjs/task/u7q3x9f2";
var MARK = "RECPROBE-LINE";
var SCREEN_MARK = "RECPROBE-SCREEN";

function pub(o) { mqtt.publish(BASE + "/proberep", JSON.stringify(o), 0, 0); }
function box(kind) {
    var s;
    try { s = sys.blackbox(kind); } catch (e) { return "ERR:" + e; }
    return s === null ? "" : "" + s;
}

mqtt.onConnect(function () {
    var reset = "?";
    try { reset = "" + (JSON.parse("" + sys.blackbox()).reset || "?"); } catch (e) {}

    if (reset === "panic") {
        /* The panic boot: did the panic-time capture land, and did the
           device survive taking it (we are running, so it booted). */
        var lb = box("lastboot");
        if (lb.length > 6000) lb = lb.slice(lb.length - 6000);
        pub({ phase: "rec-panic-result", reset: reset,
              sawScreenMark: lb.indexOf(SCREEN_MARK) >= 0,
              sawLineMark: lb.indexOf(MARK) >= 0,
              tail: lb });
        setTimeout(function () { sys.stop("probe_rec"); }, 400);
        return;
    }

    var ok = [], bad = [];
    var chk = function (n, c) { (c ? ok : bad).push(n); };

    var id = term.create({ name: "rec0", mode: "vt", cols: 40, rows: 6 });
    chk("create", id > 0);
    chk("default off", term.record(id) === 0);

    /* OFF: scroll lines past. None of this may reach the box. */
    var i;
    for (i = 0; i < 20; i++)
        term.feed(id, "OFFSECRET-" + i + "\r\n");

    setTimeout(function () {
        var b = box("live");
        chk("off is inert", b.indexOf("OFFSECRET") < 0);

        chk("record(on)", term.record(id, true) === 0);
        chk("reports on", term.record(id) === 1);
        for (i = 0; i < 20; i++)
            term.feed(id, MARK + "-" + i + "\r\n");
        term.feed(id, SCREEN_MARK + " visible now\r\n");

        setTimeout(function () {
            var b2 = box("live");
            chk("recorded lines in box", b2.indexOf(MARK) >= 0);
            chk("off-period text still absent", b2.indexOf("OFFSECRET") < 0);
            chk("manual screen capture", term.recordScreen(id) === 0);

            setTimeout(function () {
                var b3 = box("live");
                chk("screen mark in box", b3.indexOf(SCREEN_MARK) >= 0);
                var h = sys.heap();
                pub({ phase: "rec-device", ok: ok, bad: bad,
                      psram: h[1], internal: h[0] });

                /* Now the risky half: leave recording ON and panic. The
                   panic path must capture this screen without taking the
                   device into a double panic. */
                pub({ phase: "rec-panic-arm",
                      note: "recording ON, panicking in ~600ms" });
                setTimeout(function () { sys.panic(); }, 600);
            }, 700);
        }, 700);
    }, 700);
});

net.onReady(function (token) { mqtt.connect(token); });
