// dev-slot probe: phase-4 VT term on the device, minus real ssh.
// The pipe-to-a-live-session half needs credentials and a human; this
// covers everything else the phase-4 plumbing added, on hardware:
// a vt-mode term, escape parsing through the UI-task drain, the
// underline/strike attrs reaching the blitter, show/hide + repaint
// (Defect A), the pipe error paths, onReply, and resize.
// What to read: every phase reports pass/fail per check; "ok" arrays
// list what held. Restore dev_idle.js afterwards.
"use strict";
sys.setAppName("probe_vt");

var BASE = "esp32p4-mqjs/task/u7q3x9f2";

function pub(o) { mqtt.publish(BASE + "/proberep", JSON.stringify(o), 0, 0); }

mqtt.onConnect(function () {
    var ok = [], bad = [];
    var chk = function (name, cond) { (cond ? ok : bad).push(name); };

    var id = term.create({ name: "vt0", mode: "vt", cols: 40, rows: 12 });
    chk("create>0", id > 0);
    if (id <= 0) { pub({ phase: "vt", fatal: "create", rc: id }); return; }

    chk("show", term.show(id, { x: 0, y: 0, w: 400, h: 300 }) === 0);

    /* plain text, SGR underline, a cursor move, then more text: the
       parse happens on the UI task, so nothing is visible until a frame
       has run. */
    term.feed(id, "PLAIN-A\r\n");
    term.feed(id, "\x1b[4mUNDER-B\x1b[24m \x1b[9mSTRIKE-C\x1b[29m\r\n");
    term.feed(id, "\x1b[5;3HMOVED-D");

    /* pipe error paths: a bogus handle must be a negative error, never a
       throw and never a crash. */
    var rcBogus = -999;
    try { rcBogus = term.pipe(id, 99999); } catch (e) { rcBogus = "threw:" + e; }
    chk("pipe(bogus)<0", typeof rcBogus === "number" && rcBogus < 0);
    var rcUnpipe = -999;
    try { rcUnpipe = term.unpipe(id); } catch (e2) { rcUnpipe = "threw:" + e2; }
    chk("unpipe(unpiped) no throw", typeof rcUnpipe === "number");

    var replies = 0;
    chk("onReply set", term.onReply(id, function () { replies++; }) === 0);
    term.feed(id, "\x1b[6n");          /* DSR: device status report */

    setTimeout(function () {
        var s = term.snapshot(id);
        s = (s === null) ? "" : "" + s;
        chk("snapshot non-null", s.length > 0);
        chk("plain text parsed", s.indexOf("PLAIN-A") >= 0);
        chk("sgr text parsed", s.indexOf("UNDER-B") >= 0 && s.indexOf("STRIKE-C") >= 0);
        chk("cursor move honoured", s.indexOf("MOVED-D") >= 0);
        chk("no escape bytes in snapshot", s.indexOf("\x1b") < 0);
        chk("DSR reply delivered", replies > 0);
        pub({ phase: "vt-parse", ok: ok, bad: bad, replies: replies,
              snapLen: s.length });

        /* hide then re-show: Defect A's repaint path. Nothing here can
           see pixels; what it proves is that the sequence does not
           error or crash and the content survives. */
        var rcHide = term.show(id, null);
        var rcReshow = term.show(id, { x: 0, y: 0, w: 400, h: 300 });
        var rcResize = term.resize(id, 60, 20);

        setTimeout(function () {
            var ok2 = [], bad2 = [];
            var chk2 = function (n, c) { (c ? ok2 : bad2).push(n); };
            chk2("hide ok", rcHide === 0);
            chk2("reshow ok", rcReshow === 0);
            chk2("resize ok", rcResize === 0);
            var s2 = term.snapshot(id);
            s2 = (s2 === null) ? "" : "" + s2;
            chk2("content survived hide/show/resize", s2.indexOf("PLAIN-A") >= 0);
            var h = sys.heap();
            var rcClose = term.close(id);
            chk2("close ok", rcClose === 0);
            pub({ phase: "vt-lifecycle", ok: ok2, bad: bad2,
                  psram: h[1], internal: h[0] });
            setTimeout(function () { sys.stop("probe_vt"); }, 400);
        }, 700);
    }, 700);
});

net.onReady(function (token) { mqtt.connect(token); });
