// P4d host test, dev-slot half (see p4_clip_b.js for the slot-2 half):
//   timeout 3 /tmp/run_pc tools/p4_clip_a.js tools/p4_clip_b.js
// Verifies the typed clipboard across two contexts. Success markers:
//   "CLIP A PRE empty", "CLIP B GOT text/plain|hello-from-a",
//   "CLIP A GOT number|42", "CLIP GET OK", "CLIPTEST DONE"
// Failure marker (setter must never see its own change):
//   "CLIP A GOT text/plain|..."
"use strict";

var g0 = clipboard.get();
print("CLIP A PRE " + (g0 === undefined ? "empty" : "nonempty"));

clipboard.onChange(function (data, type) {
    print("CLIP A GOT " + type + "|" + data);
    if (type !== "number")
        return;
    var g = clipboard.get();
    if (g && g.data === "42" && g.type === "number")
        print("CLIP GET OK");
    else
        print("FAIL: get() mismatch: " + JSON.stringify(g));
    print("CLIPTEST DONE");
});

/* A is the setter here: B's onChange must fire, A's must not */
if (!clipboard.set("hello-from-a", "text/plain"))
    print("FAIL: set returned false");
