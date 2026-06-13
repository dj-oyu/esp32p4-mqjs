// dev-slot probe: install named apps from the store catalog, launch them,
// report progress over MQTT (<base>/proberep), then stop (auto-rerun held
// until the next push). Restore the dev slot with dev_idle.js afterwards.
"use strict";
sys.setAppName("probe_store");

var BASE = "esp32p4-mqjs/task/u7q3x9f2";
var WANT = ["circuit", "reading"];

function pub(o) { mqtt.publish(BASE + "/proberep", JSON.stringify(o), 0, 0); }

function selfStop() {
    var a = sys.apps();
    for (var i = 0; i < a.length; i++)
        if (a[i].name === "probe_store") sys.stop(a[i].slot);
}

mqtt.onConnect(function () {
    var i;
    for (i = 0; i < WANT.length; i++) sys.install(WANT[i]);
    var n = 0;
    var t = setInterval(function () {
        n++;
        var st = sys.store();
        var have = {};
        for (var j = 0; j < st.length; j++)
            if (st[j].installed) have[st[j].name] = 1;
        var all = true;
        for (var k = 0; k < WANT.length; k++)
            if (!have[WANT[k]]) all = false;
        if (!all && n <= 15) return;
        clearInterval(t);
        pub({ phase: "install", have: have, tries: n });
        for (var m = 0; m < WANT.length; m++)
            if (have[WANT[m]]) sys.launch(WANT[m]);
        setTimeout(function () {
            var a = sys.apps(), run = [];
            for (var q = 0; q < a.length; q++)
                if (a[q].running) run.push(a[q].name);
            pub({ phase: "launch", running: run });
            setTimeout(selfStop, 1000);
        }, 5000);
    }, 1000);
});
net.onReady(function (token) { mqtt.connect(token); });
