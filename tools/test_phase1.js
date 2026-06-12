// PC smoke for the Phase 1 name-based app API (app-manager migration):
//   /tmp/run_pc tools/test_phase1.js tools/test_phase1_peer.js
// The peer runs as a concurrent app named "test_phase1_peer"; this
// script exercises sys.start/open/focus/stop by name against it and
// the unknown-name false paths. Exits non-zero JS rc on failure.
"use strict";

var fails = 0;
function ok(c, m) {
    if (c) print("ok " + m);
    else { fails++; print("FAIL " + m); }
}

ok(typeof sys.onStop === "function", "sys.onStop exists (Phase 4)");

/* unknown names: every name form answers false, never throws */
ok(sys.start("no_such_app_x") === false, "start(unknown) -> false");
ok(sys.open("no_such_app_x") === false, "open(unknown) -> false");
ok(sys.focus("no_such_app_x") === false, "focus(unknown) -> false");
ok(sys.stop("no_such_app_x") === false, "stop(unknown) -> false");

/* apps(): kind is exposed (slot stays compat-only) */
var a = sys.apps();
ok(a.length >= 2, "apps() sees this app + peer");
var peer = null;
for (var i = 0; i < a.length; i++) {
    ok(typeof a[i].kind === "string", "kind on '" + a[i].name + "'");
    ok(typeof a[i].evictable === "boolean",
       "evictable on '" + a[i].name + "'");
    if (a[i].name === "test_phase1_peer") peer = a[i];
}
ok(!!peer, "peer visible by name");
ok(peer && peer.evictable === true, "plain app is evictable (policy)");

if (peer) {
    ok(sys.start("test_phase1_peer") === true, "start(running) idempotent");
    ok(sys.focus("test_phase1_peer") === true, "focus(name) -> true");
    ok(sys.open("test_phase1_peer") === true, "open(running) -> true");
    ok(sys.stop("test_phase1_peer") === true, "stop(name) -> true");
    var b = sys.apps(), still = false;
    for (var j = 0; j < b.length; j++)
        if (b[j].name === "test_phase1_peer") still = true;
    ok(!still, "stopped peer gone from apps()");
    /* no registry/littlefs source on the PC runner */
    ok(sys.start("test_phase1_peer") === false, "restart w/o source -> false");
}

print(fails ? "PHASE1 SELFTEST: " + fails + " FAILED"
            : "PHASE1 SELFTEST: ALL PASS");
