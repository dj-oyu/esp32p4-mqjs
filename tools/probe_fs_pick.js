// dev-slot probe: exercises fs.pick(opts, cb) — the native picker's JS
// entry point (docs/native-editor-spec.md §A.4 fs_picker, §A.5 grant table,
// §A.6 native surface). Unlike probe_fs.js this needs a HUMAN TAP to
// finish: fs.pick opens a native modal (UI task, not this JS worker) and
// this probe just waits for someone to choose a file in it.
//
// STATUS AS WRITTEN (2026-08-26): js_fs_pick + dispatch_fsgrant exist in
// components/mqjs/mqjs_runtime.c and, as read directly on that date, are
// consistent with each other (this is another agent's concurrent work in
// the same tree — components/** is out of scope for this probe's file,
// read-only check only, and it can drift again after this was written):
//   - `fs.pick({mode, title, start, exts, name}, cb)`. mode "open" ->
//     READ, "save" -> CREATE|WRITE (js_fs_pick).
//   - dispatch_fsgrant pushes args in reverse (arg0 = last pushed) as
//     JS_NewInt32(token) then JS_NewString(granted), JS_Call(ctx, 2) —
//     so the callback sees exactly cb(grant, path). For fs.pick it reads
//     the path from p->from_pick / s_fs_pick_land.vpath (set by
//     fs_pick_cb on the UI task) rather than p->root; a rejected/expired
//     landing collapses to the same "denied" shape as fs.request (token
//     0, path "").
// None of the above is executed by anything in this probe or checked by
// a build — components/mqjs/mqjs_runtime.c was link-tested (undefined
// refs into term_core/fs_core/fs_picker, which this session was not
// asked to wire up) but not run, so treat this as "read off the current
// source", not "observed on a device". This probe still reports whatever
// shape the callback actually delivers (see picked_cb below) so a real
// run settles it instead of trusting the reading.
//
// Run it from the dev slot; watch <base>/proberep on MQTT, or the serial
// log if no broker is up. It opens the picker ~immediately (or as soon as
// the broker connects) and then just sits there until someone taps a file
// or backs out.
"use strict";
sys.setAppName("fspickprobe");

var TOPIC = null;
var out = function (tag, obj) {
    obj.t = tag;
    var line = JSON.stringify(obj);
    print("[fspickprobe] " + line);
    if (TOPIC)
        mqtt.publish(TOPIC, line);
};

/* mquickjs はブロック内の function 宣言を呼び出し時点まで巻き上げない
   ので (examples/README / tools の落とし穴)、この後の関数もすべて
   var 束縛で書く。 */
var onPick = function (a, b) {
    var grant = null, vpath = null;
    if (typeof b !== "undefined") {
        /* cb(grant, vpath) — 期待している形 */
        grant = a;
        vpath = b;
    } else if (a && typeof a === "object") {
        /* cb({...}) — 予備の形 */
        grant = (typeof a.grant !== "undefined") ? a.grant : a.token;
        vpath = (typeof a.vpath !== "undefined") ? a.vpath : a.path;
    } else {
        /* cb(grant) だけ — パスが分からない */
        grant = a;
    }
    out("picked_cb", { grant: grant, vpath: vpath,
                       argType: typeof a, arg2Type: typeof b });

    if (!grant) {
        out("cancelled", {});
        sys.stop("fspickprobe");
        return;
    }
    if (!vpath) {
        /* シグネチャの推測が外れた: grant は来たがどのパスかが分からない。
           fs.read はパスを要るので、ここで止まる。上のコメントブロック
           参照。 */
        out("no_path", { note: "cb shape did not carry a path; see header comment" });
        sys.stop("fspickprobe");
        return;
    }
    try {
        var body = fs.read(vpath, { length: 256 });
        out("read", { vpath: vpath, ok: true, len: body.length,
                      head: body.slice(0, 64) });
    } catch (e) {
        out("read", { vpath: vpath, ok: false, err: "" + e });
    }
    out("done", {});
    sys.stop("fspickprobe");
};

var run = function () {
    if (typeof fs.pick !== "function") {
        out("no_binding", { picked: false });
        sys.stop("fspickprobe");
        return;
    }
    out("waiting", { mode: "open" });
    try {
        var started = fs.pick({ mode: "open" }, onPick);
        if (!started) {
            out("begin_failed", {});
            sys.stop("fspickprobe");
        }
    } catch (e2) {
        out("begin_threw", { err: "" + e2 });
        sys.stop("fspickprobe");
    }
};

var didStart = false;
var start = function () {
    if (didStart)
        return;
    didStart = true;
    run();
};

/* JS の mqtt は自動接続されない: net.onReady の token を渡して初めて
   繋がる (docs の net.onReady capability)。publish はセッションが
   確立してからでないと落ちるので、報告は onConnect まで待つ。 */
net.onReady(function (token) {
    mqtt.connect(token);
});
mqtt.onConnect(function () {
    TOPIC = net.topic("proberep");
    start();
});

/* ブローカーが居なくてもシリアルには出す。fs.pick はネイティブモーダルを
   開くのでどのみち人のタップが要る — 8 秒待ってから未接続でも始める。 */
setTimeout(start, 8000);
