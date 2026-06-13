// dev-slot probe: device E2E for Phase 4 (LRU eviction + onStop).
// At boot all 4 workers are busy (launcher + this probe + 2 autostart
// apps) — sys.start("circuit") must now EVICT the LRU background app
// instead of failing. Cleans up after itself; restore dev_idle.js
// afterwards. The final self-stop publishes onStop("user") — delivery
// is best-effort (the mqtt client dies right after the handler).
"use strict";
sys.setAppName("probe_p4");

var BASE = "esp32p4-mqjs/task/u7q3x9f2";

function pub(o) { mqtt.publish(BASE + "/proberep", JSON.stringify(o), 0, 0); }
function names() {
    var a = sys.apps(), r = [], i;
    for (i = 0; i < a.length; i++) r.push(a[i].name);
    return r;
}
function kinds() {
    var a = sys.apps(), r = [], i;
    for (i = 0; i < a.length; i++)
        r.push(a[i].name + ":" + a[i].kind + (a[i].evictable ? "+ev" : ""));
    return r.join(",");
}

sys.onStop(function (r) {
    pub({ phase: "p4-onstop", reason: r });
});

mqtt.onConnect(function () {
    /* fill the 4th worker first if this boot left one free (the
       eviction path needs a genuinely full house) */
    var filled = names().length < 4 ? sys.start("ssh_vt") : "already-full";
    var before = names();
    pub({ phase: "p4-pre", apps: kinds(), filled: filled,
          full: before.length === 4 });
    /* all 4 workers busy -> this start must evict the LRU bg app */
    var rc = sys.start("circuit");
    var after = names();
    var victim = null;
    for (var i = 0; i < before.length; i++) {
        var still = false;
        for (var j = 0; j < after.length; j++)
            if (after[j] === before[i]) still = true;
        if (!still) victim = before[i];
    }
    pub({ phase: "p4-evict", rc: rc, victim: victim, apps: kinds() });
    setTimeout(function () {
        var s1 = sys.stop("circuit");
        var rs = victim ? sys.start(victim) : false;
        pub({ phase: "p4-cleanup", stopCircuit: s1, restoreVictim: rs,
              apps: kinds() });
        setTimeout(function () {
            pub({ phase: "p4-done", apps: kinds() });
            sys.stop("probe_p4"); /* -> onStop("user") above */
        }, 800);
    }, 1500);
});
net.onReady(function (token) { mqtt.connect(token); });
