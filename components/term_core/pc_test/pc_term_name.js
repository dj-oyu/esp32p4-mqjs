// pc_term_name.js — the name key, from inside one app (§3.1, §8).
//
//   var id = term.create({name: "ssh0", persist: true, mode: "vt"});
//                                    // 同名+同一 owner なら再アタッチ
//
// A full re-attach needs the owner to have STOPPED — that is the
// app-manager teardown hook (§3.1), which run_pc does not exercise for the
// script it is running. What one app CAN observe about the key, and what
// this script therefore checks, is:
//
//   * a second create for a name that is still LIVE is not a second handle
//     to the same term (term_registry.h: TERM_ERR_EXISTS, "not a second
//     handle");
//   * after close, the same name is available again, and the id that comes
//     back is a NEW one — the generation moved (§3.1's ABA guard);
//   * the old id does not address the new term.
//
// The DETACHED -> re-attach half belongs to the host suite
// (test_registry_persist.c) and, on the device, to the teardown hook.
"use strict";

var fails = 0;
var ok = function (c, m) {
    if (c) print("ok " + m);
    else { fails++; print("FAIL " + m); }
};
var call = function (m, fn) {
    try { return fn(); }
    catch (e) { fails++; print("FAIL " + m + " THREW: " + e); return undefined; }
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

var first = 0;
var second = 0;
var other = 0;

ok(typeof term === "object" && term !== null, "term object exists");
if (typeof term !== "object" || term === null) { finish(); } else {

steps.push(function () {
    first = call("create", function () {
        return term.create({ name: "ssh0", persist: true, mode: "log" });
    });
    ok(typeof first === "number" && first > 0, "create(persist) returns an id");
    call("log", function () { return term.log(first, "FIRST_SESSION"); });
});

steps.push(function () {
    var dup = call("create same name while live", function () {
        return term.create({ name: "ssh0", persist: true, mode: "log" });
    });
    ok(!(typeof dup === "number" && dup > 0 && dup !== first),
       "a live name does not hand out a second, different term");
    ok(dup !== first || typeof dup !== "number",
       "...nor a second handle to the same one (term_registry.h EXISTS)");
});

steps.push(function () {
    other = call("create another name", function () {
        return term.create({ name: "ssh1", persist: true, mode: "log" });
    });
    ok(typeof other === "number" && other > 0, "a different name is a different term");
    ok(other !== first, "and a different id");
    call("log other", function () { return term.log(other, "OTHER_SESSION"); });
});

steps.push(function () {
    var a = call("snapshot first", function () { return term.snapshot(first); });
    var b = call("snapshot other", function () { return term.snapshot(other); });
    ok(typeof a === "string" && a.indexOf("FIRST_SESSION") >= 0,
       "each term keeps its own content");
    ok(typeof a === "string" && a.indexOf("OTHER_SESSION") < 0,
       "...and only its own");
    ok(typeof b === "string" && b.indexOf("OTHER_SESSION") >= 0,
       "the second term likewise");
});

steps.push(function () {
    call("close first", function () { return term.close(first); });
});

steps.push(function () {
    second = call("re-create the freed name", function () {
        return term.create({ name: "ssh0", persist: true, mode: "log" });
    });
    ok(typeof second === "number" && second > 0,
       "the name is available again after close");
    ok(second !== first,
       "the new term has a NEW id: the generation moved (§3.1 ABA)");
});

steps.push(function () {
    var s = call("snapshot new", function () { return term.snapshot(second); });
    ok(typeof s === "string" && s.indexOf("FIRST_SESSION") < 0,
       "a closed session's content does not follow its name (§3.1)");
    var old = call("snapshot old id", function () { return term.snapshot(first); });
    ok(typeof old !== "string" || old.indexOf("FIRST_SESSION") < 0,
       "and the old id addresses nothing");
    call("cleanup", function () { term.close(second); return term.close(other); });
});

pump();
}
