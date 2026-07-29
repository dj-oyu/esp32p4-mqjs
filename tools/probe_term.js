// dev-slot probe: the device half of docs/term-design.md §8 / the phase-2
// manifest. The host suites prove the registry is CORRECT and the run_pc
// suites prove the bindings answer instead of throwing; only the device can
// say what the lazy bring-up actually costs in PSRAM, what a log/feed/
// snapshot costs in microseconds on a 360MHz P4, and whether the memory
// comes back when a term is closed (§3.1 stage 2 is asynchronous, so
// "closed" and "freed" are different moments and this probe measures both).
//
// Reports on <base>/proberep as a sequence of small JSON messages, then
// stops. Restore dev_idle.js afterwards.
//
// RUN IT TWICE. The last phase is persist re-attach (§3.1, the tmux model),
// which by definition needs the owner to have stopped once: run 1 plants a
// marker in a persist term and leaves it attached, the app teardown hook
// detaches it, and run 2 (after a reboot or a re-push) reports whether the
// same name came back with its scrollback intact.
//
// What to read in the output:
//   consts     which term.* error constants exist and their values. §8 says
//              every failure is a negative term_err_t; a name reported as
//              "undef" is a binding/ROM-regen gap, not a passing test. The
//              PC build reports POST and TRUNC as "undef" while INVAL..
//              TIMEOUT (-1..-12) are all there — and POST is reachable from
//              JS (term_registry.h: resize/show return it when the UI queue
//              refuses the job), so an app can receive a code it cannot name.
//   bringup    us = wall time of the FIRST term.* call, which is where the
//              port install + registry init + system console + reaper task
//              + UI frame hook all happen (manifest "Bring-up"). psram =
//              bytes that call cost. Budget: ~110KB for the console term
//              plus this one (§4.1).
//   perterm    the SECOND create, i.e. one term's allocation on its own
//              (I4: one block per term) with bring-up already paid.
//   ops        per-call microseconds. log/feed only copy into the byte ring
//              (§5 — the parse happens later on the UI task), so they
//              should be cheap and flat; snapshot/read join the UI task at
//              a frame boundary (§7.2) and are therefore frame-scale, not
//              microsecond-scale. bad = calls that returned an error.
//   drain      whether text written with log/feed is visible in a snapshot
//              a few frames later. "0" here means the UI frame hook is not
//              running — the single most likely device-only failure.
//   errs       what the closed/junk-id paths returned, and `anomalies` —
//              the calls whose answer did not match the shape their contract
//              mandates (§8: errors are negative return values, or null for
//              snapshot/read, and nothing throws). An empty `anomalies` is
//              the pass. Known from the PC run, expected to repeat here:
//              read(id, 0, -1) answers with CONTENT although
//              term_registry.h says "n <= 0 is TERM_ERR_INVAL".
//   leak       PSRAM at: before any term, after the closes, +1.5s, +4s.
//              The last one is past the 3s quiesce deadline, so a slot that
//              is still holding memory there is a ZOMBIE (§3.1) — real, and
//              deliberately not a use-after-free, but worth seeing.
//   reattach   run 1: "planted". run 2: found=true means the persist term
//              survived the owner stopping and re-attached by (owner, name).
"use strict";
sys.setAppName("probe_term");

var BASE = "esp32p4-mqjs/task/u7q3x9f2";
var MARKER = "TERM_REATTACH_MARKER";

var seq = 0;
function pub(o) {
    o.n = ++seq;
    mqtt.publish(BASE + "/proberep", JSON.stringify(o), 0, 0);
}

function heap() { return sys.heap(); }          /* [internal, psram, lvgl] */
function psram() { return sys.heap()[1]; }

/* performance.now() is 1ms, and the interesting calls here are µs-scale
   (examples/skk_test.js §measurement). sys.micros() is an absolute count,
   so past ~17.9 min of uptime it leaves mquickjs' short-int range and each
   call allocates — the differences stay correct, the measurement just gets
   slightly heavier. */
var HAVE_US = (typeof sys.micros === "function");
function nowUs() { return HAVE_US ? sys.micros() : (performance.now() * 1000) | 0; }

/* A term-level error is a return value, never an exception (§8): a negative
   number from the void calls, null from snapshot/read. Anything else from a
   deliberately-bad call is the finding. */
function bad(r) { return r === null || (typeof r === "number" && r < 0); }

function timeit(n, fn) {
    var t0 = nowUs(), nbad = 0;
    for (var i = 0; i < n; i++)
        if (bad(fn(i))) nbad++;
    var us = nowUs() - t0;
    return { n: n, us: us, per: ((us * 100 / n) | 0) / 100, bad: nbad };
}

var base = [0, 0, 0];
var idLog = 0, idVt = 0;
var W = 0, H = 0;

/* --------------------------------------------------------------- phase 0 */
/* Constants and the baseline heap, both BEFORE any term.* CALL. Reading
   term.BAD_ID is a property fetch on the class object and does not bring
   the subsystem up; only the calls in phase 1 do. Nothing above this point
   may print() either — the print sink tees into the console term. */
var ERRC = ["INVAL", "NOT_READY", "BAD_ID", "STALE", "NOT_OWNER", "DYING",
            "NO_SLOT", "NO_MEM", "EXISTS", "BUSY", "MODE", "TIMEOUT",
            "POST", "TRUNC"];

function phase0() {
    var have = (typeof term === "object" && term !== null);
    var c = {};
    if (have) {
        for (var i = 0; i < ERRC.length; i++) {
            var v = term[ERRC[i]];
            c[ERRC[i]] = (typeof v === "number") ? v : "undef";
        }
    }
    base = heap();
    if (typeof ui === "object" && typeof ui.size === "function") {
        var s = ui.size();
        W = s[0] | 0; H = s[1] | 0;
    }
    pub({ phase: "start", haveTerm: have, us: HAVE_US, canvas: [W, H],
          internal: base[0], psram: base[1], lvgl: base[2], consts: c });
    if (!have) {
        setTimeout(done, 500);
        return;
    }
    setTimeout(phase1, 100);
}

/* --------------------------------------------------------------- phase 1 */
/* The first call pays for the whole subsystem; the second pays for one
   term. Reporting them separately is the only way to attribute the PSRAM. */
function phase1() {
    var t0 = nowUs();
    idLog = term.create({ name: "plog", mode: "log" });
    var us1 = nowUs() - t0;
    var h1 = heap();

    t0 = nowUs();
    idVt = term.create({ name: "pvt", mode: "vt", cols: 80, rows: 24 });
    var us2 = nowUs() - t0;
    var h2 = heap();

    pub({ phase: "bringup", id: idLog, us: us1,
          psram: base[1] - h1[1], internal: base[0] - h1[0] });
    pub({ phase: "perterm", id: idVt, us: us2,
          psram: h1[1] - h2[1], internal: h1[0] - h2[0] });

    if (idLog <= 0 || idVt <= 0) {     /* §8: create returns a positive id */
        pub({ phase: "abort", why: "create failed", idLog: idLog, idVt: idVt });
        setTimeout(phase5, 100);
        return;
    }
    setTimeout(phase2, 100);
}

/* --------------------------------------------------------------- phase 2 */
/* show + the two write paths, timed. The VT term gets a real rectangle so
   that the frame hook has something to blit — a term nobody shows exercises
   the ring and the parser but never the renderer. */
function phase2() {
    var rect = { x: 0, y: 0, w: (W > 0 ? W : 720), h: (H > 0 ? (H >> 1) : 480) };
    var t0 = nowUs();
    var rShow = term.show(idVt, rect);
    var usShow = nowUs() - t0;

    var l = timeit(200, function (i) { return term.log(idLog, "probe line " + i); });
    var f = timeit(50, function (i) {
        return term.feed(idVt, "\x1b[3" + (i % 8) + "mvt " + i + "\x1b[0m\r\n");
    });
    var m = timeit(1, function () { return term.log(idLog, "MARK_DRAIN"); });

    pub({ phase: "ops", show: { rc: rShow, us: usShow }, log: l, feed: f,
          mark: m.us, psramNow: psram() });
    setTimeout(phase3, 400);     /* let several frames drain and blit (§5) */
}

/* --------------------------------------------------------------- phase 3 */
/* Reads. Also the only honest way to ask, from JS, whether the UI frame
   hook is alive: bytes written in phase 2 can only appear here if somebody
   drained the ring and ran the parser. */
function phase3() {
    var t0 = nowUs();
    var sl = term.snapshot(idLog);
    var usSnapLog = nowUs() - t0;

    t0 = nowUs();
    var sv = term.snapshot(idVt);
    var usSnapVt = nowUs() - t0;

    t0 = nowUs();
    var rd = term.read(idLog, 0, 20);
    var usRead = nowUs() - t0;

    /* Non-mutating (§7.2): the same read twice must give the same answer. */
    var rd2 = term.read(idLog, 0, 20);

    pub({ phase: "drain",
          snapLog: (sl === null ? -1 : sl.length),
          snapVt: (sv === null ? -1 : sv.length),
          sawMark: (sl !== null && sl.indexOf("MARK_DRAIN") >= 0),
          sawVt: (sv !== null && sv.indexOf("vt ") >= 0),
          read: (rd === null ? -1 : rd.length),
          readStable: (rd === rd2),
          us: { snapLog: usSnapLog, snapVt: usSnapVt, read: usRead } });

    /* resize is posted to the UI task and content is carried over (§4.1);
       the geometry a read sees may lag by a frame, hence the delay. */
    t0 = nowUs();
    var r1 = term.resize(idVt, 100, 30);
    var usResize = nowUs() - t0;
    pub({ phase: "resize", rc: r1, us: usResize });
    setTimeout(phase4, 300);
}

/* --------------------------------------------------------------- phase 4 */
/* The false paths. §8: no term.* call throws for a terminal-level error, so
   the whole block runs without a try — if something throws, the exception in
   the log IS the result. */
function phase4() {
    var sv = term.snapshot(idVt);
    pub({ phase: "postresize", snapVt: (sv === null ? -1 : sv.length),
          survived: (sv !== null && sv.indexOf("vt ") >= 0) });

    var tmp = term.create({ name: "ptmp", mode: "log" });
    term.log(tmp, "SOON_GONE");
    var rcClose = term.close(tmp);

    var e = {
        closedLog: term.log(tmp, "x"),
        closedFeed: term.feed(tmp, "x"),
        closedResize: term.resize(tmp, 40, 10),
        closedShow: term.show(tmp, { x: 0, y: 0, w: 10, h: 10 }),
        closedSnap: term.snapshot(tmp),
        closedRead: term.read(tmp, 0, 4),
        closedClose: term.close(tmp),
        zeroLog: term.log(0, "x"),
        negLog: term.log(-1, "x"),
        hugeLog: term.log(2147483647, "x"),
        noName: term.create({}),
        emptyName: term.create({ name: "" }),
        dupLive: term.create({ name: "plog", mode: "log" }),
        badRead: term.read(idLog, 0, -1),
        badResize: term.resize(idVt, 0, 0),
        hugeResize: term.resize(idVt, 99999, 99999)
    };
    /* Each false path has ONE contract-mandated shape, so the verdict is a
       comparison against it and not "did it look error-ish":
         neg  = a negative term_err_t (§8)
         null = snapshot/read, whose success value is a string (§8)
         ok   = TERM_OK by contract (closing a DYING term is a documented
                no-op, term_registry.h §close)
         post = returns as soon as the job is QUEUED (term_registry.h
                §resize), so TERM_OK here says nothing about the geometry —
                out-of-range is judged later, on the UI task, where no return
                value reaches JS. Recorded, never scored.
       Anything in `anomalies` is a finding: a call that did not answer the
       way the contract it implements says it must. */
    var keys = ["closedLog", "closedFeed", "closedResize", "closedShow",
                "closedSnap", "closedRead", "closedClose", "zeroLog",
                "negLog", "hugeLog", "noName", "emptyName", "dupLive",
                "badRead", "badResize", "hugeResize"];
    var want = ["neg", "neg", "neg", "neg",
                "null", "null", "ok", "neg",
                "neg", "neg", "neg", "neg", "neg",
                "neg", "neg", "post"];
    var out = { phase: "errs", tmp: tmp, rcClose: rcClose, vals: {},
                anomalies: [] };
    for (var i = 0; i < keys.length; i++) {
        var v = e[keys[i]], w = want[i], good;
        out.vals[keys[i]] = (v === null) ? "null"
                          : (typeof v === "string") ? ("str:" + v.length) : v;
        if (w === "neg")       good = (typeof v === "number" && v < 0);
        else if (w === "null") good = (v === null);
        else if (w === "ok")   good = (v === 0);
        else                   good = true;
        if (!good) out.anomalies.push(keys[i] + "=" + out.vals[keys[i]]);
    }
    pub(out);

    /* dupLive should be EXISTS, not a second handle (§3.1) — and the
       original must still work after every false path above. */
    var still = term.log(idLog, "STILL_ALIVE");
    pub({ phase: "afterErrs", log: still, psramNow: psram() });
    setTimeout(phase5, 200);
}

/* --------------------------------------------------------------- phase 5 */
/* Close, then watch the memory come back. close() is stage 1 only and
   returns immediately (§3.1): the block is released on a later reaper pass,
   and a slot whose acks never arrive becomes a ZOMBIE at the 3s deadline and
   keeps its memory on purpose. So a single reading right after close proves
   nothing — take four. */
var leak = {};
function phase5() {
    term.show(idVt, null);                        /* leave the screen alone */
    var rcA = term.close(idLog);
    var rcB = term.close(idVt);
    leak = { phase: "leak", rcA: rcA, rcB: rcB,
             base: base[1], atClose: psram() };
    setTimeout(function () {
        leak.after1500 = psram();
        setTimeout(function () {
            leak.after4000 = psram();
            leak.heldVsBase = leak.base - leak.after4000;
            leak.internalNow = sys.heap()[0];
            pub(leak);
            setTimeout(phase6, 100);
        }, 2500);
    }, 1500);
}

/* --------------------------------------------------------------- phase 6 */
/* Persist re-attach (§3.1). Nothing here can be concluded in one run: the
   term is left ATTACHED and the app-manager teardown hook turns it into a
   DETACHED slot when this probe stops. The next run's create of the same
   (owner, name) is the re-attach, and the marker is how it is recognised. */
function phase6() {
    var id = term.create({ name: "pkeep", mode: "log", persist: true });
    var snap = (id > 0) ? term.snapshot(id) : null;
    var found = (snap !== null && snap.indexOf(MARKER) >= 0);
    pub({ phase: "reattach", id: id, found: found,
          snap: (snap === null ? -1 : snap.length),
          note: found ? "re-attached with scrollback"
                      : "planted; run this probe again",
          psramNow: psram() });
    if (id > 0) term.log(id, MARKER);      /* left open on purpose */
    setTimeout(done, 800);
}

function done() {
    pub({ phase: "end", psramNow: sys.heap()[1], internal: sys.heap()[0] });
    setTimeout(function () { sys.stop("probe_term"); }, 500);
}

mqtt.onConnect(function () { phase0(); });

net.onReady(function (token) { mqtt.connect(token); });
