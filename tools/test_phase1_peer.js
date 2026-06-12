// Peer app for tools/test_phase1.js: stays alive via the signal
// handler so the main script can start/focus/stop it by name.
"use strict";
sys.onSignal(function (v, from) {});
print("peer up");
