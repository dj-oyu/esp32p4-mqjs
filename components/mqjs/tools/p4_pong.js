// P4a host test, slot-2 half (see p4_ping.js). Replies to every signal
// with value+1; prints PONG FOREGROUND when focused after the rally.
"use strict";

var ticks = 0;
setInterval(function () { ticks++; }, 50); /* per-app timers keep running */

sys.onSignal(function (v, from) {
    var n = parseInt(v);
    print("pong got " + n + " from " + from + " (ticks=" + ticks + ")");
    sys.signal(from, "" + (n + 1));
});
sys.onForeground(function () { print("PONG FOREGROUND (ticks=" + ticks + ")"); });
