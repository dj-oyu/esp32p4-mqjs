// pc_term_basic.js — the term.* round trip under run_pc (design §8).
//
//   var id = term.create({name, persist, mode});
//   term.log(id, str); term.snapshot(id); term.read(id, from, n);
//   term.show(id, {x,y,w,h}); term.close(id);
//
// Written against docs/term-design.md §8 and components/term_core/
// term_registry.h. Where §8 does not fix a return shape (term.read), the
// test accepts any of the plausible ones and asserts on the CONTENT.
//
//   run:  /tmp/run_pc components/term_core/pc_test/pc_term_basic.js
"use strict";

var fails = 0;
var ok = function (c, m) {
    if (c) print("ok " + m);
    else { fails++; print("FAIL " + m); }
};

/* §8 says errors are return values, not exceptions — so a throw is itself
 * a failure, in every one of these scripts. */
var call = function (m, fn) {
    try { return fn(); }
    catch (e) { fails++; print("FAIL " + m + " THREW: " + e); return undefined; }
};

/* term.read's return shape is not fixed by §8; take the text out of
 * whichever of the reasonable shapes turns up. */
var readText = function (r) {
    if (typeof r === "string") return r;
    if (r && typeof r === "object") {
        if (typeof r.text === "string") return r.text;
        if (r.lines && typeof r.lines.join === "function") return r.lines.join("\n");
    }
    return "";
};

var steps = [];
var finish = function () {
    print(fails ? "TERM PC SELFTEST: " + fails + " FAILED"
                : "TERM PC SELFTEST: ALL PASS");
};
var pump = function () {
    if (!steps.length) { finish(); return; }
    var f = steps.shift();
    f();
    setTimeout(pump, 60);
};

var id = 0;

ok(typeof term === "object" && term !== null, "term object exists (§8)");
if (typeof term !== "object" || term === null) { finish(); } else {

ok(typeof term.create === "function", "term.create");
ok(typeof term.log === "function", "term.log");
ok(typeof term.feed === "function", "term.feed");
ok(typeof term.show === "function", "term.show");
ok(typeof term.resize === "function", "term.resize");
ok(typeof term.snapshot === "function", "term.snapshot");
ok(typeof term.read === "function", "term.read");
ok(typeof term.close === "function", "term.close");

steps.push(function () {
    id = call("create", function () {
        return term.create({ name: "basic", persist: false, mode: "log" });
    });
    ok(typeof id === "number" && id > 0, "create returns a positive id");
    /* §3.1: the id packs (generation << 3) | slot, so it is never a raw
     * slot number and never below the slot count. */
    ok(id >= 8, "the id is not a raw slot number (§3.1)");
});

steps.push(function () {
    call("show", function () {
        return term.show(id, { x: 0, y: 0, w: 320, h: 200 });
    });
    call("log", function () { return term.log(id, "hello term"); });
    /* Enough lines that the first one is off the screen at any plausible
     * row count — §4.1 caps a term at 53 rows. */
    var i;
    for (i = 0; i < 200; i++) call("log", function () { return term.log(id, "L" + i); });
});

steps.push(function () {
    var s = call("snapshot", function () { return term.snapshot(id); });
    ok(typeof s === "string", "snapshot returns a string (§7.2)");
    ok(typeof s === "string" && s.indexOf("L199") >= 0,
       "the newest line is on screen");
    ok(typeof s === "string" && s.indexOf("hello term") < 0,
       "the oldest line has scrolled off the screen");
});

steps.push(function () {
    var t = readText(call("read", function () { return term.read(id, 0, 4); }));
    ok(t.indexOf("hello term") >= 0,
       "read() reaches the scrollback the screen lost (§3.2)");
    ok(t.indexOf("L0") >= 0, "read() returns the oldest chunk first");
});

steps.push(function () {
    var again = call("snapshot again", function () { return term.snapshot(id); });
    var once = call("snapshot once", function () { return term.snapshot(id); });
    ok(again === once, "reading twice gives the same answer (§7.2 non-mutating)");
});

steps.push(function () {
    call("close", function () { return term.close(id); });
});

steps.push(function () {
    var s = call("snapshot after close", function () { return term.snapshot(id); });
    ok(typeof s !== "string" || s.indexOf("L199") < 0,
       "a closed id yields no content (§3.1 the id is dead)");
});

pump();
}
