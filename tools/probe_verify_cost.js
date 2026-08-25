// dev-slot probe: Ed25519 検証 1 回のコストを、既存のバインディングだけで測る。
//
// 動機: カード上のアプリは「読むたびに crypto_sign_open」で検証している
// (docs/filer-storage-design.md §13.3)。この 1 回がいくらかで 2 つの判断が
// 変わる:
//
//   (a) apps/ の不変条件を「chokepoint で守る」から「ロード時に毎回検証」へ
//       二重化する案が成立するか。1 秒/本なら autostart 3 本で起動が 3 秒
//       伸びるので成立しない。10ms なら成立する。
//   (b) 既に出荷した card_apps.c が、ストア画面を開くたびにカード上の
//       全ファイルを検証している。カードに 20 本置いた人の画面が何秒
//       固まるか。
//
// tweetnacl はサイズ最適化された実装で速度最適化されていないので、
// 事前予想は「数百 ms 〜 1 秒/本」。ここが外れているなら Fable の
// 「5〜30ms」が正しく、二重化案が生き返る。
//
// 測り方: 署名が通らないダミーの .mjsa を N 本置いて sys.store() を計る。
// crypto_sign_open は失敗する入力でも全計算をしてから比較するので、
// 失敗ケースのコストは成功ケースと同じ。差分から 1 本あたりを出す。
//
// 非破壊。置いたダミーは最後に消す。
"use strict";
sys.setAppName("vcost");

var TOPIC = null;
var out = function (tag, obj) {
    obj.t = tag;
    var line = JSON.stringify(obj);
    print("[vcost] " + line);
    if (TOPIC)
        mqtt.publish(TOPIC, line);
};

var N = 8;

var run = function () {
    var sd = null;
    var vols = fs.volumes();
    for (var i = 0; i < vols.length; i++)
        if (vols[i].id === "sd" && vols[i].mounted)
            sd = vols[i];
    if (!sd) {
        out("skip", { why: "no mounted sd" });
        sys.stop("vcost");
        return;
    }

    fs.request({ path: "/sd", write: true, reason: "verify cost probe" },
               function (g) {
        if (!g) {
            out("skip", { why: "no grant" });
            sys.stop("vcost");
            return;
        }
        var dir = "/sd/apps";
        var made = [];
        try {
            try { fs.mkdir(g, dir); } catch (eD) { /* 既にある */ }

            /* ---- 基準線: カードに何も置かない状態の sys.store() ---- */
            var b0 = performance.now();
            var base = sys.store();
            var b1 = performance.now();
            out("baseline", { ms: (b1 - b0).toFixed(1), rows: base.length });

            /* ---- ダミーを N 本置く ---- */
            var pad = "0123456789abcdef".repeat(4);          /* 64B の偽署名 */
            var body = "// @app vcostX\n// @title cost\n" +
                       "0123456789".repeat(20);              /* 本体 ~220B */
            for (var k = 0; k < N; k++) {
                var p = dir + "/vcost" + k + ".mjsa";
                fs.write(g, p, pad + body);
                made.push(p);
            }

            /* ---- N 本ぶんの検証込みで測る (2 回、2 回目を採る) ---- */
            var t0 = performance.now();
            sys.store();
            var t1 = performance.now();
            sys.store();
            var t2 = performance.now();

            var withN = t2 - t1;
            var per = (withN - (b1 - b0)) / N;
            out("verify", {
                n: N,
                first_ms: (t1 - t0).toFixed(1),
                second_ms: withN.toFixed(1),
                per_file_ms: per.toFixed(1),
                /* この 2 つが実際の判断材料 */
                autostart3_ms: (per * 3).toFixed(0),
                card20_ms: (per * 20).toFixed(0)
            });
        } catch (e) {
            out("verify", { err: "" + e });
        }
        for (var m = 0; m < made.length; m++) {
            try { fs.remove(g, made[m], false); } catch (eR) { }
        }
        out("done", { cleaned: made.length });
        sys.stop("vcost");
    });
};

var started = false;
var start = function () {
    if (started)
        return;
    started = true;
    run();
};

net.onReady(function (token) { mqtt.connect(token); });
mqtt.onConnect(function () {
    TOPIC = net.topic("proberep");
    start();
});
setTimeout(start, 8000);
