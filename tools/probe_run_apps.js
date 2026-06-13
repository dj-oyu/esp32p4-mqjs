// dev-slot probe 2: free a user slot (stop ssh_vt), launch circuit and
// reading one at a time, verify each stays alive ~4s, restore ssh_vt,
// report each step over MQTT (<base>/proberep), then stop.
"use strict";
sys.setAppName("probe_run");

var BASE = "esp32p4-mqjs/task/u7q3x9f2";

function pub(o) { mqtt.publish(BASE + "/proberep", JSON.stringify(o), 0, 0); }

function slotOf(name) {
    var a = sys.apps();
    for (var i = 0; i < a.length; i++)
        if (a[i].name === name && a[i].running) return a[i].slot;
    return -1;
}
function runningNames() {
    var a = sys.apps(), run = [];
    for (var i = 0; i < a.length; i++)
        if (a[i].running) run.push(a[i].name);
    return run;
}

mqtt.onConnect(function () {
    var s = slotOf("ssh_vt");
    if (s >= 0) sys.stop(s);
    pub({ phase: "freed", stopped: s >= 0 ? "ssh_vt" : "(none)" });

    sys.launch("circuit");
    setTimeout(function () {
        var ok1 = slotOf("circuit") >= 0;
        pub({ phase: "circuit", alive: ok1, running: runningNames() });
        var c = slotOf("circuit");
        if (c >= 0) sys.stop(c);

        sys.launch("reading");
        setTimeout(function () {
            var ok2 = slotOf("reading") >= 0;
            pub({ phase: "reading", alive: ok2, running: runningNames() });
            var r = slotOf("reading");
            if (r >= 0) sys.stop(r);

            sys.launch("ssh_vt"); /* 元の状態に戻す */
            setTimeout(function () {
                pub({ phase: "done", running: runningNames() });
                var me = slotOf("probe_run");
                if (me >= 0) sys.stop(me);
            }, 1500);
        }, 4000);
    }, 4000);
});
net.onReady(function (token) { mqtt.connect(token); });
