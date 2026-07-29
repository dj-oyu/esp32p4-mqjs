// dev-slot probe: the PSRAM-arena tax, measured. Run the SAME script on
// two builds and compare per-pass microseconds:
//
//   A: CONFIG_MQJS_DEV_ARENA_KB=96, MQJS_DEV_ARENA_INTERNAL=n  (PSRAM)
//   B: CONFIG_MQJS_DEV_ARENA_KB=96, MQJS_DEV_ARENA_INTERNAL=y  (L2 SRAM)
//
// Compare 96 vs 96, NEVER against the 256 KB default — arena size sets
// GC cadence and would confound everything. The boot log line
// "dev arena probe: 96 KB, internal L2|PSRAM" says which build this is;
// the reported l2free also self-labels (the internal build runs ~96 KB
// lower).
//
// Tests, 3 passes each (deterministic, LCG-shuffled — identical work on
// both builds):
//   call  - function call + integer arithmetic (frames + bytecode)
//   walk  - pointer-chase through 1200 shuffled linked nodes (~58 KB
//           live) — the latency-bound case the whole question is about
//   str   - build + slice strings (allocation-light string traffic)
//   churn - allocate-and-drop objects. GC-CONFOUNDED by design: arena
//           pressure differs run to run; read it as flavor, not verdict.
//
// Caveat for interpretation: a 96 KB working set may sit largely in the
// cache hierarchy either way. If A==B here, SRAM-placing typical
// foreground apps buys nothing — which IS the answer being sought.
// Restore dev_idle.js afterwards.
"use strict";
sys.setAppName("probe_abench");

var BASE = "esp32p4-mqjs/task/u7q3x9f2";

function pub(o) { mqtt.publish(BASE + "/proberep", JSON.stringify(o), 0, 0); }

function selfStop() {
    var a = sys.apps();
    for (var i = 0; i < a.length; i++)
        if (a[i].name === "probe_abench") sys.stop(a[i].slot);
}

/* deterministic shuffle source */
var seed = 0x1b6f3a9d;
function lcg() {
    seed = (seed * 1103515245 + 12345) & 0x7fffffff;
    return seed;
}

function add2(a, b) { return a + b; }

function testCall() {
    var acc = 0;
    for (var i = 0; i < 200000; i++)
        acc = add2(acc, i) & 0xffffff;
    return acc;
}

var NODES = 1200;
var chain = null;
function buildChain() {
    var nodes = [];
    for (var i = 0; i < NODES; i++)
        nodes.push({ v: i, next: null });
    /* Fisher-Yates with the LCG: a shuffled traversal order defeats any
       prefetch and makes every hop a dependent load */
    var order = [];
    for (i = 0; i < NODES; i++) order.push(i);
    for (i = NODES - 1; i > 0; i--) {
        var j = lcg() % (i + 1);
        var t = order[i]; order[i] = order[j]; order[j] = t;
    }
    for (i = 0; i < NODES - 1; i++)
        nodes[order[i]].next = nodes[order[i + 1]];
    nodes[order[NODES - 1]].next = nodes[order[0]];
    chain = nodes[order[0]];
}

function testWalk() {
    var p = chain, acc = 0;
    for (var i = 0; i < 400000; i++) {
        acc = (acc + p.v) & 0xffffff;
        p = p.next;
    }
    return acc;
}

function testStr() {
    var acc = 0;
    for (var i = 0; i < 4000; i++) {
        var s = "x" + i + "-" + (i * 7) + "-abcdefghijklmnop";
        acc = (acc + s.length + s.slice(3, 9).length) & 0xffffff;
    }
    return acc;
}

function testChurn() {
    var keep = null;
    for (var i = 0; i < 20000; i++)
        keep = { a: i, b: i * 2, c: keep && 0 };
    return keep.a;
}

var TESTS = [
    ["call", testCall], ["walk", testWalk],
    ["str", testStr], ["churn", testChurn]
];

function runAll() {
    buildChain();
    var h = sys.heap();
    pub({ start: true, l2free: h[3], psfree: h[1] });
    var results = {};
    for (var pass = 1; pass <= 3; pass++) {
        for (var t = 0; t < TESTS.length; t++) {
            var name = TESTS[t][0], fn = TESTS[t][1];
            var t0 = sys.micros();
            var acc = fn();
            var dt = sys.micros() - t0;
            if (!results[name]) results[name] = [];
            results[name].push(dt);
            pub({ bench: name, pass: pass, us: dt, acc: acc });
        }
    }
    pub({ done: results });
    setTimeout(selfStop, 1000);
}

mqtt.onConnect(function () {
    /* let boot/net settle so the first pass isn't fighting bringup */
    setTimeout(runAll, 3000);
});

net.onReady(function (token) { mqtt.connect(token); });
