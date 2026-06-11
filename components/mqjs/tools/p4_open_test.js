// P4b host test: the open-request path through the launcher.
//   /tmp/run_pc tools/p4_open_test.js ../../examples/launcher.js
// Sends {"op":"open","app":"tools/p4_pong.js"} to the launcher, which
// must sys.launch the file into a free slot and focus it. Success
// markers: "open request sent: true", "foreground -> 'p4_pong'",
// and an apps dump containing p4_pong.
"use strict";

setTimeout(function () {
    var ok = sys.signal("launcher",
                        JSON.stringify({ op: "open", app: "tools/p4_pong.js" }));
    print("open request sent: " + ok);
}, 100);

setTimeout(function () {
    print("apps: " + JSON.stringify(sys.apps()));
}, 600);
