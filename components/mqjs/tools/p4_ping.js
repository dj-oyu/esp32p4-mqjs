// P4a host test, dev-slot half (see p4_pong.js for the slot-2 half):
//   /tmp/run_pc tools/p4_ping.js tools/p4_pong.js
// Verifies signal ping-pong across two contexts and the foreground
// switch lifecycle. Success markers in the output:
//   "PINGPONG DONE", "PING BACKGROUND", "PONG FOREGROUND"
"use strict";

sys.onSignal(function (v, from) {
    var n = parseInt(v);
    print("ping got " + n + " from " + from);
    if (n >= 6) {
        print("PINGPONG DONE");
        sys.focus(2); /* -> ping onBackground, pong onForeground */
        return;
    }
    sys.signal("p4_pong", "" + (n + 1));
});
sys.onBackground(function () { print("PING BACKGROUND"); });

if (!sys.signal("p4_pong", "1"))
    print("FAIL: pong not running");
