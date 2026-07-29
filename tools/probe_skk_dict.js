// dev-slot probe: which dictionary skk.open() picked, and what a lookup
// costs through it. This is the device half of docs/skk-ime-design.md S8
// — the host tests prove the search is CORRECT, only the device can say
// whether the mmap'd `jisyo` partition opens at all and what the CRC
// pass costs. Reports on <base>/proberep, then stops. Restore
// dev_idle.js afterwards.
//
// What to read in the output:
//   dict     "part:jisyo" = bamboo/pine from the partition, "builtin" =
//            plum from flash rodata. If it says builtin, the partition
//            is empty or failed to open and the fallback took over.
//   nasi/ari heading counts — ML is 44401/4349, M is 6934/1412
//   levels   sampled-tree depth; lines/lookup is levels + 1
//   loadUs   mmap + CRC32 of the whole image, paid once by skk.open()
//   probes   64-byte lines the searches touched, summed
"use strict";
sys.setAppName("probe_skk");

var BASE = "esp32p4-mqjs/task/u7q3x9f2";

function pub(o) { mqtt.publish(BASE + "/proberep", JSON.stringify(o), 0, 0); }

function selfStop() { sys.stop("probe_skk"); }

mqtt.onConnect(function () {
    var ime = skk.open();
    pub({ phase: "open", stats: skk.stats(ime) });

    /* Verified against both images with `skk_prep.py inspect --lookup`:
       ざせつ and りゅうどう are in ML and NOT in M, そがい has one
       candidate in M and three in ML, ちくじ is in both. So the four
       answers together say which dictionary actually got searched — not
       just which one skk.stats() claims. */
    var probes = ["Zasetsu", "Ryuudou", "Sogai", "Chikuji"];
    var out = [];
    for (var i = 0; i < probes.length; i++) {
        skk.reset(ime);
        var w = probes[i];
        for (var j = 0; j < w.length; j++) skk.key(ime, w.charAt(j));
        skk.key(ime, " ");                       /* convert */
        out.push({ roma: w, pre: skk.preedit(ime), cands: skk.candidates(ime) });
    }
    pub({ phase: "lookup", got: out, stats: skk.stats(ime) });

    skk.close(ime);
    setTimeout(selfStop, 500);
});

net.onReady(function (token) { mqtt.connect(token); });
