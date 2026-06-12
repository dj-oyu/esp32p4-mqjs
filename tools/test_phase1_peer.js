// Peer app for tools/test_phase1.js: stays alive via the signal
// handler so the main script can start/focus/stop it by name. The
// onStop hook prints the reason ("user" expected: the main script
// stops us by name) — visible in the run output.
"use strict";
sys.onSignal(function (v, from) {});
sys.onStop(function (r) { print("peer onStop: " + r); });
print("peer up");
