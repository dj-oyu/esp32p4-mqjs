// pc_term_errors.js — §8's error convention, from the JS side.
//
//   "エラー規約: 全 API は呼び出しごとに generation + owner を検証し、
//    close 済み/再利用済み/他アプリの id は例外ではなく エラー戻り値で返す
//    (ターミナルのエラーでアプリを落とさない)"
//
// So the property under test is not "which value comes back" — the design
// leaves the binding free to answer falsy or negative — but that NOTHING
// throws, nothing aborts the app, and the script keeps running to the end.
// The final PASS line is itself the strongest assertion here: it is only
// printed if the interpreter survived every one of these calls.
"use strict";

var fails = 0;
var ok = function (c, m) {
    if (c) print("ok " + m);
    else { fails++; print("FAIL " + m); }
};

/* Returns true when the call answered instead of throwing. */
var answers = function (m, fn) {
    try { fn(); print("ok " + m + " answered"); return true; }
    catch (e) { fails++; print("FAIL " + m + " THREW: " + e); return false; }
};

/* An error result must not be mistaken for success: §8 wants a falsy or
 * negative answer, never a fresh usable id. */
var notAnId = function (v) {
    return !(typeof v === "number" && v > 0);
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

ok(typeof term === "object" && term !== null, "term object exists");
if (typeof term !== "object" || term === null) { finish(); } else {

steps.push(function () {
    id = term.create({ name: "err", mode: "log" });
    ok(typeof id === "number" && id > 0, "a term to close later");
    term.log(id, "BEFORE_CLOSE");
});

steps.push(function () {
    term.close(id);
});

steps.push(function () {
    /* Every §8 entry point, on an id that is dead. None may throw. */
    answers("log(closed)", function () { return term.log(id, "after"); });
    answers("feed(closed)", function () { return term.feed(id, "after"); });
    answers("resize(closed)", function () { return term.resize(id, 40, 10); });
    answers("show(closed)", function () { return term.show(id, { x: 0, y: 0, w: 10, h: 10 }); });
    answers("snapshot(closed)", function () { return term.snapshot(id); });
    answers("read(closed)", function () { return term.read(id, 0, 4); });
    answers("close(closed)", function () { return term.close(id); });

    var s = term.snapshot(id);
    ok(typeof s !== "string" || s.indexOf("BEFORE_CLOSE") < 0,
       "a closed term hands back no content");
});

steps.push(function () {
    /* Ids that were never issued. §3.1's gate makes these errors, and I5
     * makes the error a value. */
    var junk = [0, -1, 1, 7, 2147483647, 123456];
    var i;
    for (i = 0; i < junk.length; i++) {
        answers("log(junk " + junk[i] + ")", function () { return term.log(junk[i], "x"); });
        answers("snapshot(junk " + junk[i] + ")", function () { return term.snapshot(junk[i]); });
        answers("close(junk " + junk[i] + ")", function () { return term.close(junk[i]); });
    }
});

steps.push(function () {
    /* Malformed arguments. A terminal error must not kill an app (I5). */
    ok(notAnId(term.create()), "create() with no options is not an id");
    ok(notAnId(term.create({})), "create({}) with no name is not an id");
    ok(notAnId(term.create({ name: "" })), "create({name:\"\"}) is not an id");
    ok(notAnId(term.create({ name: "waytoolongaterminalname_beyond_the_field" })),
       "an over-long name is rejected, not truncated (§3.1)");
    answers("log(id, undefined)", function () { return term.log(id, undefined); });
    answers("read(id, -1, -1)", function () { return term.read(id, -1, -1); });
    answers("resize(id, 0, 0)", function () { return term.resize(id, 0, 0); });
    answers("resize(id, 99999, 99999)", function () { return term.resize(id, 99999, 99999); });
});

steps.push(function () {
    /* The app is still alive and still able to make a working term: the
     * point of "errors do not kill the app". */
    var fresh = term.create({ name: "after_all", mode: "log" });
    ok(typeof fresh === "number" && fresh > 0,
       "the runtime survived every false path and still works");
    term.log(fresh, "STILL_ALIVE");
    steps.push(function () {
        var s = term.snapshot(fresh);
        ok(typeof s === "string" && s.indexOf("STILL_ALIVE") >= 0,
           "and the new term behaves normally");
        term.close(fresh);
    });
});

pump();
}
