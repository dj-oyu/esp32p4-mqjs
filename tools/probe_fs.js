// dev-slot probe: the device half of docs/filer-storage-design.md.
//
// run_pc can only prove that the fs.* bindings parse and that a board with
// no volumes answers "no storage" — every interesting claim in the design
// needs the real thing: does slot 0 come up next to the C6 on slot 1, does
// a 4-bit HS card actually mount, does UTF-8 give back Japanese filenames,
// and does the removable-volume story (probe -> auto-unmount -> dead grant)
// behave when the card is physically pulled.
//
// Reports on <base>/proberep as small JSON messages, then stops. It runs in
// the dev slot, which is auto-granted (design §7: the dev slot already has
// camera.scanQr / sys.blackbox / system.*), so NO consent screen appears
// here — that path is exercised by the files app, not by this probe.
//
// RUN IT TWICE, pulling the card between runs:
//   run 1  card in    -> volumes/rw/roundtrip all report ok
//   run 2  card out   -> "sd" reports mounted:false, and the write path
//                        reports the "not mounted" error rather than hanging
//
// Restore tools/dev_idle.js afterwards.
"use strict";
sys.setAppName("fsprobe");

var TOPIC = null;
var out = function (tag, obj) {
    obj.t = tag;
    var line = JSON.stringify(obj);
    print("[fsprobe] " + line);
    if (TOPIC)
        mqtt.publish(TOPIC, line);
};

var run = function () {
    /* ---- 1. どのボリュームが登録されているか ---------------------
       ここが「ボード差はデータ」の観測点。Tab5 なら internal+sd の 2 本、
       Stamp-P4 なら internal 1 本になり、コードは同じ。 */
    var vols = fs.volumes();
    var names = [];
    for (var i = 0; i < vols.length; i++) {
        names.push(vols[i].id);
        out("vol", {
            id: vols[i].id,
            label: vols[i].label,
            fstype: vols[i].fstype,
            mounted: vols[i].mounted,
            removable: vols[i].removable,
            total: vols[i].total,
            free: vols[i].free
        });
    }
    out("volumes", { n: vols.length, ids: names.join(",") });

    /* ---- 2. 内蔵の一覧 (apps/ には必ず何か居る) ------------------- */
    try {
        var t0 = performance.now();
        var ls = fs.list("/internal/apps");
        var t1 = performance.now();
        out("list_internal", { n: ls.length, more: !!ls.more,
                               ms: (t1 - t0).toFixed(1),
                               first: ls.length ? ls[0].name : "" });
    } catch (e) {
        out("list_internal", { err: "" + e });
    }

    /* ---- 3. サンドボックス: 抜け道が塞がっているか ---------------- */
    /* 注意: この節は実機でしか意味がない。run_pc の fs.list は空配列を
       返すだけのスタブなので、PC で回すと全部 "PASSED-THROUGH" に見える
       — 検出器が居ないだけで、穴が開いているわけではない。 */
    var escapes = ["/internal/../littlefs", "/littlefs/apps", "/",
                   "/internal/apps/../../..", "/nosuchvol"];
    for (var k = 0; k < escapes.length; k++) {
        var got = "PASSED-THROUGH";      // これが出たら穴
        try {
            fs.list(escapes[k]);
        } catch (e2) {
            got = "" + e2;
        }
        out("escape", { path: escapes[k], got: got });
    }

    /* ---- 4. grant なしの書き込みは弾かれるか --------------------- */
    var denied = "NOT-THROWN";           // これが出たら穴
    try {
        fs.write(0, "/internal/probe.txt", "x");
    } catch (e3) {
        denied = "" + e3;
    }
    out("nogrant", { got: denied });

    /* ---- 5. 書き込み往復 ----------------------------------------
       dev スロットなので同意画面は出ず、callback が即座に grant を返す。
       "sd" があればそちらを、無ければ内蔵を使う。 */
    var target = null;
    for (var m = 0; m < vols.length; m++)
        if (vols[m].id === "sd" && vols[m].mounted)
            target = "sd";
    if (!target)
        target = "internal";

    fs.request({ path: "/" + target, write: true, reason: "probe" },
               function (g) {
        if (!g) {
            out("grant", { vol: target, ok: false });
            sys.stop("fsprobe");
            return;
        }
        out("grant", { vol: target, ok: true });
        var dir  = "/" + target + "/fsprobe";
        /* 日本語のファイル名。FATFS を UTF-8 API にした効果はここに出る
           (CP437 のままだと SD 側でここが化ける)。 */
        var file = dir + "/テスト.txt";
        var body = "hello ファイラ\n2 行目\n";
        try {
            try { fs.mkdir(g, dir); } catch (eDir) { /* 既にある */ }
            var w0 = performance.now();
            fs.write(g, file, body);
            var w1 = performance.now();
            var back = fs.read(file);
            var st = fs.stat(file);
            out("roundtrip", {
                vol: target,
                write_ms: (w1 - w0).toFixed(1),
                same: back === body,
                size: st ? st.size : -1,
                bytes: body.length
            });
            /* 名前が化けていないかは一覧側でも見る */
            var ls2 = fs.list(dir);
            out("relist", { n: ls2.length,
                            name: ls2.length ? ls2[0].name : "" });
            fs.remove(g, file, false);
            fs.remove(g, dir, false);
            out("cleanup", { ok: fs.stat(file) === undefined });
        } catch (e4) {
            out("roundtrip", { vol: target, err: "" + e4 });
        }
        out("done", {});
        sys.stop("fsprobe");
    });
};

var started = false;
var start = function () {
    if (started)
        return;
    started = true;
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

/* ブローカーが居なくてもシリアルには出す */
setTimeout(start, 8000);
