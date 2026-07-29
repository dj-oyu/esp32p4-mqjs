// pc_term_probe.js — is `term` usable in this run_pc build?
//
// The runner uses the printed line to decide whether it must link the host
// port shim (pc_port_shim.c). Not a test: it asserts nothing, it reports.
//
//   TERMPROBE: missing       — no term object (bindings / ROM regen pending)
//   TERMPROBE: create-failed — term.create did not return an id
//   TERMPROBE: no-drain      — id issued, but nothing reaches the screen
//   TERMPROBE: ready         — a log/snapshot round trip works
"use strict";

var report = function (s) { print("TERMPROBE: " + s); };

if (typeof term !== "object" || term === null) {
    report("missing");
} else {
    var id = term.create({ name: "probe", mode: "log" });
    if (typeof id !== "number" || id <= 0) {
        report("create-failed");
    } else {
        term.log(id, "PROBEMARK");
        setTimeout(function () {
            var s = term.snapshot(id);
            if (typeof s === "string" && s.indexOf("PROBEMARK") >= 0) report("ready");
            else report("no-drain");
            term.close(id);
        }, 250);
    }
}
