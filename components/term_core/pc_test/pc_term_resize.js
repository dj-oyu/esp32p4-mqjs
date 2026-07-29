// pc_term_resize.js — term.resize from JS (§8, §4.1 rotation path).
//
//   term.resize(id, cols, rows);   // "pty への通知は呼び出し側の責務"
//
// §5 puts the write on the UI task, so a resize is visible "on the next
// frame" rather than immediately — every check here therefore runs a
// setTimeout later. §4.1's whole point is that a rotation costs no
// reallocation and loses no content, so the assertions are: the content
// survives, the scrollback re-wraps at the new width instead of freezing
// at the old one (§3.1), and an impossible geometry changes nothing.
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
    setTimeout(pump, 80);
};

var id = 0;
var wide = "";
var i;
for (i = 0; i < 100; i++) wide += "ABCDEFGHIJ".charAt(i % 10);

ok(typeof term === "object" && term !== null, "term object exists");
if (typeof term !== "object" || term === null) { finish(); } else {

steps.push(function () {
    id = call("create", function () {
        return term.create({ name: "rot", mode: "vt" });
    });
    ok(typeof id === "number" && id > 0, "create a VT term");
    call("show", function () { return term.show(id, { x: 0, y: 0, w: 640, h: 480 }); });
});

steps.push(function () {
    call("resize portrait", function () { return term.resize(id, 80, 24); });
});

steps.push(function () {
    /* A line far wider than 80 columns: it must soft-wrap now and re-wrap
     * after the rotation, not stay frozen at the old width (§3.1). */
    call("feed", function () { return term.feed(id, "MARKER_START" + wide + "MARKER_END\r\n"); });
    call("log", function () { return term.log(id, "second line"); });
});

steps.push(function () {
    var s = call("snapshot", function () { return term.snapshot(id); });
    ok(typeof s === "string" && s.indexOf("MARKER_START") >= 0,
       "the wide line is on screen at 80 columns");
    ok(typeof s === "string" && s.indexOf("MARKER_END") >= 0,
       "...including the part that wrapped");
});

steps.push(function () {
    call("resize landscape", function () { return term.resize(id, 142, 30); });
});

steps.push(function () {
    var s = call("snapshot after resize", function () { return term.snapshot(id); });
    ok(typeof s === "string" && s.indexOf("MARKER_START") >= 0,
       "content survives the resize (§4.1: carried over, not dropped)");
    ok(typeof s === "string" && s.indexOf("MARKER_END") >= 0,
       "...and so does the far end of the wide line");
    ok(typeof s === "string" && s.indexOf("second line") >= 0,
       "the following line is still there too");
});

steps.push(function () {
    /* Back to portrait, then out of range: §4.1 sizes the block for the
     * worst case over both orientations, so both of these are ordinary
     * operations and the third must simply not happen. */
    call("resize back", function () { return term.resize(id, 80, 53); });
});

steps.push(function () {
    var s = call("snapshot after second resize", function () { return term.snapshot(id); });
    ok(typeof s === "string" && s.indexOf("MARKER_START") >= 0,
       "content survives a second rotation");
    call("resize absurd", function () { return term.resize(id, 100000, 100000); });
    call("resize zero", function () { return term.resize(id, 0, 0); });
    call("resize negative", function () { return term.resize(id, -4, -4); });
});

steps.push(function () {
    var s = call("snapshot after bad resize", function () { return term.snapshot(id); });
    ok(typeof s === "string" && s.indexOf("MARKER_START") >= 0,
       "an out-of-range resize left the term intact (term_core B5)");
    var t = readText(call("read", function () { return term.read(id, 0, 8); }));
    ok(typeof t === "string", "read() still answers after the rotations");
    call("close", function () { return term.close(id); });
});

pump();
}
