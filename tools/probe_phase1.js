// dev-slot probe: device E2E for the Phase 1 name-based app API.
// Starts/relegates real installed apps by NAME only, reports each step
// on <base>/proberep, then stops itself (by name). Restore dev_idle.js
// afterwards.
"use strict";
sys.setAppName("probe_p1");

var BASE = "esp32p4-mqjs/task/u7q3x9f2";

function pub(o) { mqtt.publish(BASE + "/proberep", JSON.stringify(o), 0, 0); }
function kinds() {
    var a = sys.apps(), r = [], i;
    for (i = 0; i < a.length; i++) r.push(a[i].name + ":" + a[i].kind);
    return r.join(",");
}

mqtt.onConnect(function () {
    pub({ phase: "p1-pre", apps: kinds(),
          unknown: [sys.start("nope_x"), sys.open("nope_x"),
                    sys.focus("nope_x"), sys.stop("nope_x")] });
    /* all 4 workers are busy at boot (launcher + dev + 2 autostarts):
       free one by NAME first — this is also the stop(name) test */
    var sv = sys.stop("ssh_vt");
    var st = sys.start("circuit");
    pub({ phase: "p1-start-circuit", stopSshVt: sv, rc: st, apps: kinds() });
    setTimeout(function () {
        var f = sys.focus("circuit");
        var op = sys.open("circuit"); /* open(running) = focus, true */
        pub({ phase: "p1-focus-open", focus: f, open: op, apps: kinds() });
        setTimeout(function () {
            var s1 = sys.stop("circuit");
            var rs = sys.start("ssh_vt"); /* restore the autostart app */
            var fb = sys.focus("launcher");
            pub({ phase: "p1-stop", circuit: s1, restartSshVt: rs,
                  focusLauncher: fb, apps: kinds() });
            setTimeout(function () {
                pub({ phase: "p1-done", apps: kinds() });
                sys.stop("probe_p1"); /* self-stop by name (reaper path) */
            }, 800);
        }, 1500);
    }, 1500);
});
mqtt.connect("mqtt://192.168.1.2");
