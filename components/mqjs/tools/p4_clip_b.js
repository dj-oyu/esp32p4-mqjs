// P4d host test, slot-2 half (see p4_clip_a.js). On A's set this app
// gets onChange(data, type), then answers by setting a "number" value
// (its own onChange must NOT re-fire — setter exclusion).
"use strict";

var got = 0;
print("CLIP B PRE " + (clipboard.get() === undefined ? "empty" : "nonempty"));

clipboard.onChange(function (data, type) {
    got++;
    print("CLIP B GOT " + type + "|" + data);
    if (got > 1) {
        print("FAIL: B saw its own set (echo)");
        return;
    }
    clipboard.set(42, "number"); /* numbers stringify like sys.signal */
});
