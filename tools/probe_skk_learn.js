// dev-slot probe: the personal dictionary across a REBOOT (S7).
//
// Each run converts かんじ, reports which candidate came out FIRST, then
// picks the SECOND one, commits it and calls skk.save().
//
//   1. subscribe to <base>/proberep and LEAVE IT SUBSCRIBED
//   2. push this file          -> first=幹事  chose=感じ
//   3. esptool ... --after watchdog-reset
//   4. read the NEXT report    -> first=感じ   <- learning survived
//
// The check is that the post-reboot `first` equals the pre-reboot
// `chose`. A device that never wrote the file, or wrote it and could not
// read it back, reports the dictionary's own order both times.
//
// ⚠️ DO NOT push it again after the reboot to get step 4 — a pushed dev
// task is persisted to littlefs and RE-RUNS ON BOOT, so by the time a
// second push lands, the boot run has already converted, chosen and
// saved once more. The reading is then one step stale and looks exactly
// like a lost write; that cost a round of chasing a bug that was not
// there. The boot run publishes its own report, which is the reading you
// want, and it makes the test automatic. This applies to any probe that
// measures something the probe itself changes.
//
// It deliberately keeps NO state of its own. The first version stashed
// the expectation in store.*, and THAT really did not survive — store's
// NVS commit is deferred and a watchdog reset beats the deferral, while
// the personal dictionary (fsync'd on save) came through fine.
//
// Restore dev_idle.js afterwards, or the next boot converts again.
"use strict";
sys.setAppName("probe_skl");

var BASE = "esp32p4-mqjs/task/u7q3x9f2";

function pub(o) { mqtt.publish(BASE + "/proberep", JSON.stringify(o), 0, 0); }

mqtt.onConnect(function () {
    var ime = skk.open();

    skk.reset(ime);
    var w = "Kanji";
    for (var i = 0; i < w.length; i++) skk.key(ime, w.charAt(i));
    skk.key(ime, " ");
    var c = skk.candidates(ime);

    if (c.length < 2) {
        pub({ error: "need 2 candidates for かんじ, got " + c.length });
    } else {
        skk.key(ime, " ");          /* move to the 2nd */
        skk.key(ime, "\n");         /* commit it: this is what gets learned */
        pub({ first: c[0], chose: c[1], cands: c,
              saved: skk.save(ime),
              note: "reboot; the NEXT report's `first` must be " + c[1],
              stats: skk.stats(ime) });
    }
    skk.close(ime);
    setTimeout(function () { sys.stop("probe_skl"); }, 500);
});

net.onReady(function (token) { mqtt.connect(token); });
