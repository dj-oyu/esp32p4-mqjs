// @app circuit
// @title 回路計算機
// @icon 
// @desc 部品を置くと合成抵抗・電流・電圧を導出式つきで自動計算。値は表計算セル式 (10k, R1*2) で入力。
//
// 回路計算機: 電子回路のアイデアの種を「表計算の使い心地」で実現する。
//
//  - 上のツールバーで部品 (導線/抵抗/電源) を選び、格子の隣り合う 2 点を
//    順にタップして配置 (2 点目がそのまま次の始点になるので連続配線できる)
//  - 電源 1 個 + 抵抗ネットワークを直並列に簡約し、合成抵抗 Rtot を
//    「R1 + (R2 // R3)」のような導出式つきで表示 (// = 並列)
//  - 部品をタップすると、その部品の V・I の導出式 (分圧/分流) を表示
//  - 下の表は表計算ライク: 行をタップして値セルを編集。数値 (10k, 4.7M,
//    10m) のほか他の部品を参照する式 (R1*2, 1/(1/R1+1/R2)) が書け、
//    依存する部品の値・電流・電圧はすべて自動で再計算される
//  - 回路は NVS に自動保存され、再起動後も復元される
//
// 非対応 (種の範囲): ブリッジ等の非直並列回路、複数電源、コンデンサ等。
// PC (run_pc) ではソルバ・式評価の SELFTEST が走る。
"use strict";
sys.setAppName("circuit");

var SELFTEST = false; /* true にすると PC でソルバ検証だけ走る */
var HAS_UI = !SELFTEST && ui.size()[0] !== 0;

/* ===== レイアウト定数 (720x1192 キャンバス) ===== */
var W = 720;
var SP = 80;                 /* 格子間隔 */
var GC = 9, GR = 7;          /* 格子ノード数 (列 x 行) */
var GX0 = 40, GY0 = 96;      /* 格子原点 */
var FML_Y = 584;             /* 導出式エリアの先頭 y */
var SHEET_ROW0 = 29;         /* 表のヘッダ行 (ui.cells 行番号, 1行=24px) */
var SHEET_MAX = 19;          /* 表に出せる部品行数 */

var BG = 0x101820;
var COL_WIRE = 0xC9D1D9, COL_R = 0x4FC3F7, COL_V = 0xFFD479;
var COL_SEL = 0x2ECC71, COL_DIM = 0x8899AA, COL_TXT = 0xE0E6EA;

/* ===== モデル ===== */
/* 部品 = {id, kind:"W"|"R"|"V", a, b, expr, val}
 * ノードは r*16+c で詰める (格子は 9x7 なので 16 進詰めで衝突しない) */
var comps = [];
var seq = { R: 0, V: 0 };

var mode = "R";              /* ツールバーの選択: W/R/V/DEL */
var selNode = -1;
var selComp = -1;
var editIdx = -1;            /* 編集中の部品 index (-1 = なし) */
var editBuf = "";
var ana = null;              /* 直近の解析結果 */

function nx(n) { return GX0 + (n % 16) * SP; }
function ny(n) { return GY0 + ((n / 16) | 0) * SP; }

/* ===== 数値フォーマット (工学接頭辞) ===== */
function fmt(x) {
    if (!isFinite(x)) return "?";
    if (x === 0) return "0";
    var neg = x < 0;
    var a = neg ? -x : x;
    var u = "", m = 1;
    if (a >= 1e6) { u = "M"; m = 1e-6; }
    else if (a >= 1e3) { u = "k"; m = 1e-3; }
    else if (a >= 1) { u = ""; m = 1; }
    else if (a >= 1e-3) { u = "m"; m = 1e3; }
    else if (a >= 1e-6) { u = "u"; m = 1e6; }
    else { u = "n"; m = 1e9; }
    var v = a * m;
    var s;
    if (v >= 100) s = v.toFixed(1);
    else if (v >= 10) s = v.toFixed(2);
    else s = v.toFixed(3);
    if (s.indexOf(".") >= 0) {
        var e = s.length;
        while (e > 0 && s.charAt(e - 1) === "0") e--;
        if (e > 0 && s.charAt(e - 1) === ".") e--;
        s = s.slice(0, e);
    }
    return (neg ? "-" : "") + s + u;
}

function pad(s, n) {
    if (s.length > n) return s.slice(0, n - 1) + "~";
    while (s.length < n) s += " ";
    return s;
}

/* ===== 式評価 (表計算セル: 数値 + k/M/G/m/u 接頭辞 + 部品参照) ===== */
function tokenize(src) {
    var toks = [], i = 0, n = src.length;
    while (i < n) {
        var ch = src.charAt(i);
        if (ch === " " || ch === "\t") { i++; continue; }
        if ((ch >= "0" && ch <= "9") || ch === ".") {
            var j = i;
            while (j < n) {
                var d = src.charAt(j);
                if ((d >= "0" && d <= "9") || d === ".") j++;
                else break;
            }
            var num = parseFloat(src.slice(i, j));
            if (!isFinite(num)) throw new Error("数値が読めません: " + src.slice(i, j));
            var sfx = j < n ? src.charAt(j) : "";
            if (sfx === "k") { num *= 1e3; j++; }
            else if (sfx === "M") { num *= 1e6; j++; }
            else if (sfx === "G") { num *= 1e9; j++; }
            else if (sfx === "m") { num *= 1e-3; j++; }
            else if (sfx === "u") { num *= 1e-6; j++; }
            toks.push({ t: "n", v: num });
            i = j;
            continue;
        }
        if ((ch >= "A" && ch <= "Z") || (ch >= "a" && ch <= "z")) {
            var j2 = i + 1;
            while (j2 < n) {
                var d2 = src.charAt(j2);
                if ((d2 >= "A" && d2 <= "Z") || (d2 >= "a" && d2 <= "z") ||
                    (d2 >= "0" && d2 <= "9")) j2++;
                else break;
            }
            toks.push({ t: "id", v: src.slice(i, j2) });
            i = j2;
            continue;
        }
        if ("+-*/()".indexOf(ch) >= 0) { toks.push({ t: ch }); i++; continue; }
        throw new Error("使えない文字: " + ch);
    }
    return toks;
}

function evalExpr(src, lookup) {
    var toks = tokenize(src);
    if (toks.length === 0) throw new Error("式が空です");
    var pos = 0;
    function peek() { return pos < toks.length ? toks[pos] : null; }
    function expr() {
        var v = term();
        while (peek() && (peek().t === "+" || peek().t === "-")) {
            var op = toks[pos++].t;
            var w = term();
            v = op === "+" ? v + w : v - w;
        }
        return v;
    }
    function term() {
        var v = unary();
        while (peek() && (peek().t === "*" || peek().t === "/")) {
            var op = toks[pos++].t;
            var w = unary();
            if (op === "/") {
                if (w === 0) throw new Error("0 で割っています");
                v = v / w;
            } else {
                v = v * w;
            }
        }
        return v;
    }
    function unary() {
        if (peek() && peek().t === "-") { pos++; return -unary(); }
        return prim();
    }
    function prim() {
        var tk = peek();
        if (!tk) throw new Error("式が途中で終わっています");
        pos++;
        if (tk.t === "n") return tk.v;
        if (tk.t === "id") return lookup(tk.v);
        if (tk.t === "(") {
            var v = expr();
            if (!peek() || peek().t !== ")") throw new Error("閉じカッコがありません");
            pos++;
            return v;
        }
        throw new Error("式の文法エラー");
    }
    var v = expr();
    if (pos < toks.length) throw new Error("式に余分な続きがあります");
    if (!isFinite(v)) throw new Error("計算結果が数値になりません");
    return v;
}

/* 全部品の値を式から確定 (相互参照 + 循環検出)。エラーなら文字列を返す */
function evalAll(cs) {
    var memo = {}, busy = {};
    function byId(id) {
        for (var k = 0; k < cs.length; k++)
            if (cs[k].id === id) return cs[k];
        return null;
    }
    function valOf(id) {
        if (memo["k" + id] !== undefined) return memo["k" + id];
        if (busy["k" + id]) throw new Error("循環参照: " + id);
        var c = byId(id);
        if (!c || c.kind === "W") throw new Error("未定義の名前: " + id);
        busy["k" + id] = 1;
        var v = evalExpr(c.expr, valOf);
        busy["k" + id] = 0;
        memo["k" + id] = v;
        return v;
    }
    for (var i = 0; i < cs.length; i++) {
        var c = cs[i];
        if (c.kind === "W") continue;
        try { c.val = valOf(c.id); }
        catch (e) { c.val = 0 / 0; return c.id + ": " + e.message; }
    }
    return null;
}

/* ===== union-find (導線で結ばれたノードの同一視) ===== */
function ufFind(p, x) {
    while (p[x] !== undefined) x = p[x];
    return x;
}
function ufUnion(p, a, b) {
    var ra = ufFind(p, a), rb = ufFind(p, b);
    if (ra !== rb) p[ra] = rb;
}

/* ===== 直並列簡約の木 → 導出式テキスト ===== */
function fml(t) {
    if (t.op === "leaf") return t.id;
    var parts = [];
    for (var k = 0; k < t.kids.length; k++) {
        var s = fml(t.kids[k]);
        if (t.kids[k].op !== "leaf") s = "(" + s + ")";
        parts.push(s);
    }
    return parts.join(t.op === "ser" ? " + " : " // ");
}

function pushKids(arr, t, op) {
    if (t.op === op) {
        for (var k = 0; k < t.kids.length; k++) arr.push(t.kids[k]);
    } else {
        arr.push(t);
    }
}

function markLeaves(t, per, reason) {
    if (t.op === "leaf") {
        per[t.id] = { v: 0, i: 0, p: 0, vh: reason, ih: reason };
        return;
    }
    for (var k = 0; k < t.kids.length; k++) markLeaves(t.kids[k], per, reason);
}

function sharedMidNet(a, b, deg, tA, tB) {
    var c = [a.n1, a.n2];
    for (var k = 0; k < 2; k++) {
        var x = c[k];
        if (x === tA || x === tB) continue;
        if (deg[x] !== 2) continue;
        if (b.n1 === x || b.n2 === x) return x;
    }
    return null;
}

function netsConnected(es, tA, tB) {
    var seen = {};
    seen[tA] = 1;
    var q = [tA];
    while (q.length) {
        var x = q.shift();
        for (var i = 0; i < es.length; i++) {
            var e = es[i];
            var y = null;
            if (e.n1 === x) y = e.n2;
            else if (e.n2 === x) y = e.n1;
            if (y !== null && !seen[y]) { seen[y] = 1; q.push(y); }
        }
    }
    return seen[tB] === 1;
}

/* ===== 回路解析: 式評価 → ネット化 → 直並列簡約 → 分圧/分流の伝播 ===== */
function analyze(cs) {
    var i, j, c;

    var ev = evalAll(cs);
    if (ev) return { ok: false, msg: "値エラー " + ev };

    var par = {};
    for (i = 0; i < cs.length; i++) {
        c = cs[i];
        if (c.kind === "W") ufUnion(par, "n" + c.a, "n" + c.b);
    }

    var src = null, nsrc = 0;
    for (i = 0; i < cs.length; i++)
        if (cs[i].kind === "V") { src = cs[i]; nsrc++; }
    if (nsrc === 0) return { ok: false, msg: "電源 (V) を 1 個置いてください" };
    if (nsrc > 1) return { ok: false, msg: "電源は 1 個だけ対応です" };

    var tA = ufFind(par, "n" + src.a);
    var tB = ufFind(par, "n" + src.b);
    if (tA === tB) return { ok: false, msg: "電源が導線で短絡しています" };

    var per = {};
    var es = [];
    for (i = 0; i < cs.length; i++) {
        c = cs[i];
        if (c.kind !== "R") continue;
        if (!(c.val > 0))
            return { ok: false, msg: c.id + " の値は正の数にしてください" };
        var na = ufFind(par, "n" + c.a), nb = ufFind(par, "n" + c.b);
        if (na === nb) {
            per[c.id] = { v: 0, i: 0, p: 0,
                          vh: "導線で短絡されている (電位差 0)",
                          ih: "電流は抵抗 0 の導線側を流れる (I = 0)" };
            continue;
        }
        es.push({ n1: na, n2: nb, t: { op: "leaf", r: c.val, id: c.id } });
    }
    if (es.length === 0)
        return { ok: false, msg: "抵抗 (R) を置いてください" };

    var guard = 0;
    while (es.length > 1 && guard++ < 500) {
        var changed = false;

        /* 自己ループ (簡約の結果両端が同ネット) → 電流 0 */
        for (i = es.length - 1; i >= 0; i--) {
            if (es[i].n1 === es[i].n2) {
                markLeaves(es[i].t, per, "閉ループ内で電位差なし (I = 0)");
                es.splice(i, 1);
                changed = true;
            }
        }
        if (changed) continue;

        /* 並列: 同じネット対の 2 要素 */
        var done = false;
        for (i = 0; i < es.length && !done; i++) {
            for (j = i + 1; j < es.length && !done; j++) {
                var a = es[i], b = es[j];
                if ((a.n1 === b.n1 && a.n2 === b.n2) ||
                    (a.n1 === b.n2 && a.n2 === b.n1)) {
                    var kp = [];
                    pushKids(kp, a.t, "par");
                    pushKids(kp, b.t, "par");
                    es[i] = { n1: a.n1, n2: a.n2,
                              t: { op: "par",
                                   r: a.t.r * b.t.r / (a.t.r + b.t.r),
                                   kids: kp } };
                    es.splice(j, 1);
                    done = true;
                }
            }
        }
        if (done) continue;

        var deg = {};
        for (i = 0; i < es.length; i++) {
            deg[es[i].n1] = (deg[es[i].n1] || 0) + 1;
            deg[es[i].n2] = (deg[es[i].n2] || 0) + 1;
        }

        /* 開放端 (次数 1 で端子でない) にぶら下がる要素 → 電流 0 */
        for (i = es.length - 1; i >= 0 && !changed; i--) {
            var e1 = es[i];
            if ((e1.n1 !== tA && e1.n1 !== tB && deg[e1.n1] === 1) ||
                (e1.n2 !== tA && e1.n2 !== tB && deg[e1.n2] === 1)) {
                markLeaves(e1.t, per, "開放端につながっている (I = 0)");
                es.splice(i, 1);
                changed = true;
            }
        }
        if (changed) continue;

        /* 直列: 端子以外の次数 2 ネットを共有する 2 要素 */
        done = false;
        for (i = 0; i < es.length && !done; i++) {
            for (j = i + 1; j < es.length && !done; j++) {
                var x = sharedMidNet(es[i], es[j], deg, tA, tB);
                if (x !== null) {
                    var p1 = es[i].n1 === x ? es[i].n2 : es[i].n1;
                    var p2 = es[j].n1 === x ? es[j].n2 : es[j].n1;
                    var ks = [];
                    pushKids(ks, es[i].t, "ser");
                    pushKids(ks, es[j].t, "ser");
                    es[i] = { n1: p1, n2: p2,
                              t: { op: "ser",
                                   r: es[i].t.r + es[j].t.r,
                                   kids: ks } };
                    es.splice(j, 1);
                    done = true;
                }
            }
        }
        if (done) continue;
        break;
    }

    var root = null;
    if (es.length === 1 &&
        ((es[0].n1 === tA && es[0].n2 === tB) ||
         (es[0].n1 === tB && es[0].n2 === tA)))
        root = es[0].t;
    if (!root) {
        if (es.length === 0)
            return { ok: false, msg: "電源と抵抗が導線でつながっていません" };
        if (!netsConnected(es, tA, tB))
            return { ok: false, msg: "回路が閉じていません (電源の両端まで配線を)" };
        return { ok: false, msg: "直並列に分解できない回路です (ブリッジ等は未対応)" };
    }

    var rtot = root.r;
    var itot = src.val / rtot;

    function prop(t, V, I, vh, ih) {
        if (t.op === "leaf") {
            per[t.id] = { v: V, i: I, p: V * I, vh: vh, ih: ih };
            return;
        }
        var k, kid;
        if (t.op === "ser") {
            for (k = 0; k < t.kids.length; k++) {
                kid = t.kids[k];
                var Vk = I * kid.r;
                prop(kid, Vk, I,
                     "V = I x R = " + fmt(I) + "A x " + fmt(kid.r) + "Ω = " +
                         fmt(Vk) + "V (直列分圧)",
                     "I = " + fmt(I) + "A (直列なので電流は共通)");
            }
        } else {
            for (k = 0; k < t.kids.length; k++) {
                kid = t.kids[k];
                var Ik = V / kid.r;
                prop(kid, V, Ik,
                     "V = " + fmt(V) + "V (並列なので電圧は共通)",
                     "I = V/R = " + fmt(V) + "V / " + fmt(kid.r) + "Ω = " +
                         fmt(Ik) + "A (並列分流)");
            }
        }
    }
    prop(root, src.val, itot,
         "電源電圧 " + fmt(src.val) + "V がそのまま掛かる",
         "I = V/Rtot = " + fmt(src.val) + "V / " + fmt(rtot) + "Ω = " +
             fmt(itot) + "A");
    per[src.id] = { v: src.val, i: itot, p: src.val * itot,
                    vh: "電源電圧 (設定値)",
                    ih: "I = V/Rtot = " + fmt(src.val) + "V / " + fmt(rtot) +
                        "Ω = " + fmt(itot) + "A" };

    return { ok: true, msg: "", fml: fml(root), rtot: rtot, vsrc: src.val,
             itot: itot, ptot: src.val * itot, per: per };
}

/* ===== 永続化 (NVS) ===== */
function save() {
    var arr = [];
    for (var i = 0; i < comps.length; i++) {
        var c = comps[i];
        arr.push([c.kind, c.a, c.b, c.expr, c.id]);
    }
    try {
        store.set("circ_sav", JSON.stringify({ c: arr, r: seq.R, v: seq.V }));
    } catch (e) {}
}
function load() {
    var s = store.get("circ_sav");
    if (!s) return;
    try {
        var d = JSON.parse(s);
        seq = { R: d.r || 0, V: d.v || 0 };
        comps = [];
        for (var i = 0; i < d.c.length; i++) {
            var a = d.c[i];
            comps.push({ kind: a[0], a: a[1], b: a[2], expr: a[3],
                         id: a[4], val: 0 });
        }
    } catch (e) {
        comps = [];
    }
}

/* ===== 描画 ===== */
var TOOLS = [
    { m: "W", lbl: "導線" },
    { m: "R", lbl: "抵抗" },
    { m: "V", lbl: "電源" },
    { m: "DEL", lbl: "削除" },
    { m: "CLR", lbl: "全消去" }
];

function drawToolbar() {
    for (var i = 0; i < TOOLS.length; i++) {
        var bx = 8 + i * 142;
        var active = TOOLS[i].m === mode;
        ui.rect(bx, 8, 134, 56, active ? 0x2E6BD6 : 0x222C36);
        var tw = ui.textSize(TOOLS[i].lbl)[0];
        ui.text(bx + ((134 - tw) >> 1), 26, TOOLS[i].lbl,
                active ? 0xFFFFFF : COL_TXT);
    }
}

function drawGrid() {
    for (var r = 0; r < GR; r++) {
        for (var c = 0; c < GC; c++) {
            ui.rect(GX0 + c * SP - 2, GY0 + r * SP - 2, 5, 5, 0x3A4754);
        }
    }
    if (selNode >= 0) {
        ui.rect(nx(selNode) - 7, ny(selNode) - 7, 15, 15, COL_SEL);
        ui.rect(nx(selNode) - 4, ny(selNode) - 4, 9, 9, BG);
        ui.rect(nx(selNode) - 2, ny(selNode) - 2, 5, 5, COL_SEL);
    }
    if (comps.length === 0) {
        ui.text(80, 280, "上のボタンで部品を選び、隣り合う 2 つの点を", COL_DIM);
        ui.text(80, 310, "順にタップすると配置できます", COL_DIM);
        ui.text(80, 356, "例: 電源 1 個 + 抵抗 2 個 + 導線でループを作る", 0x5A6B7A);
    }
}

function drawComp(c, sel) {
    var ax = nx(c.a), ay = ny(c.a), bx = nx(c.b), by = ny(c.b);
    var mx = (ax + bx) >> 1, my = (ay + by) >> 1;
    var col = sel ? COL_SEL :
              c.kind === "W" ? COL_WIRE : c.kind === "R" ? COL_R : COL_V;
    var horiz = ay === by;
    /* 配線 (部品はこの上に箱を重ねる) */
    if (horiz)
        ui.rect(ax < bx ? ax : bx, ay - 1, (ax < bx ? bx - ax : ax - bx) + 1, 3, col);
    else
        ui.rect(ax - 1, ay < by ? ay : by, 3, (ay < by ? by - ay : ay - by) + 1, col);
    if (c.kind === "W") return;

    var vs = fmt(c.val) + (c.kind === "R" ? "Ω" : "V");
    if (c.kind === "R") {
        if (horiz) {
            ui.rect(mx - 22, my - 10, 44, 20, col);
            ui.rect(mx - 20, my - 8, 40, 16, BG);
            ui.text(mx - 12, my - 36, c.id, col);
            ui.text(mx - 16, my + 13, vs, COL_DIM);
        } else {
            ui.rect(mx - 10, my - 22, 20, 44, col);
            ui.rect(mx - 8, my - 20, 16, 40, BG);
            ui.text(mx + 15, my - 24, c.id, col);
            ui.text(mx + 15, my + 2, vs, COL_DIM);
        }
    } else { /* V: 箱 + 極性。最初にタップした点が + */
        if (horiz) {
            ui.rect(mx - 18, my - 18, 36, 36, col);
            ui.rect(mx - 15, my - 15, 30, 30, BG);
            ui.text(ax < bx ? mx - 12 : mx + 2, my - 12, "+", col);
            ui.text(mx - 12, my - 46, c.id, col);
            ui.text(mx - 16, my + 21, vs, COL_DIM);
        } else {
            ui.rect(mx - 18, my - 18, 36, 36, col);
            ui.rect(mx - 15, my - 15, 30, 30, BG);
            ui.text(mx - 5, ay < by ? my - 14 : my - 2, "+", col);
            ui.text(mx + 23, my - 24, c.id, col);
            ui.text(mx + 23, my + 2, vs, COL_DIM);
        }
    }
}

function cap(s, n) {
    if (s.length > n) return s.slice(0, n - 1) + "…";
    return s;
}

function drawFormula() {
    var l1 = "", l2 = "", l3 = "", l4 = "";
    if (editIdx >= 0) {
        l1 = "入力中: " + comps[editIdx].id + " = " + editBuf + "_";
        l2 = "Enter で確定 / Esc で取消。例: 10k, 4.7M, R1*2, 1/(1/R1+1/R2)";
    } else if (!ana || !ana.ok) {
        l1 = ana ? ana.msg : "";
        l2 = "電源 1 個と抵抗で閉じたループを作ると自動計算します";
    } else {
        l1 = "Rtot = " + ana.fml + " = " + fmt(ana.rtot) + "Ω   (// は並列)";
        l2 = "V = " + fmt(ana.vsrc) + "V   I = " + fmt(ana.itot) +
             "A   P = " + fmt(ana.ptot) + "W";
        if (selComp >= 0 && selComp < comps.length &&
            comps[selComp].id && ana.per[comps[selComp].id]) {
            var pe = ana.per[comps[selComp].id];
            l3 = comps[selComp].id + ": " + pe.ih;
            l4 = comps[selComp].id + ": " + pe.vh +
                 "   P = " + fmt(pe.p) + "W";
        } else {
            l3 = "部品をタップすると V・I の導出式をここに表示";
        }
    }
    ui.text(8, FML_Y, cap(l1, 56), COL_TXT);
    ui.text(8, FML_Y + 24, cap(l2, 56), 0x9FB3C8);
    ui.text(8, FML_Y + 48, cap(l3, 56), COL_SEL);
    ui.text(8, FML_Y + 72, cap(l4, 56), COL_SEL);
}

function sheetComps() {
    var list = [];
    for (var i = 0; i < comps.length; i++)
        if (comps[i].kind !== "W") list.push(comps[i]);
    return list;
}

function drawSheet() {
    var hdr = pad(" ID", 5) + pad("value", 14) + pad("V", 10) +
              pad("I", 10) + pad("P", 10) + "(行タップで編集)";
    ui.cells(0, SHEET_ROW0, pad(hdr, 80), COL_DIM, 0x1A232C);
    var list = sheetComps();
    var n = list.length < SHEET_MAX ? list.length : SHEET_MAX;
    for (var k = 0; k < n; k++) {
        var c = list[k];
        var pe = ana && ana.ok ? ana.per[c.id] : null;
        var line = pad(" " + c.id, 5) +
                   pad(c.expr, 14) +
                   pad(pe ? fmt(pe.v) + "V" : "-", 10) +
                   pad(pe ? fmt(pe.i) + "A" : "-", 10) +
                   pad(pe ? fmt(pe.p) + "W" : "-", 10);
        var sel = selComp >= 0 && selComp < comps.length && comps[selComp] === c;
        var bg = sel ? 0x24435E : (k & 1) ? 0x10161C : 0x0B0E11;
        var fg = c.kind === "V" ? COL_V : COL_TXT;
        ui.cells(0, SHEET_ROW0 + 1 + k, pad(line, 80), fg, bg);
    }
    /* 余り行を掃除 (部品削除後のゴミ消し) */
    for (var e = n; e < SHEET_MAX; e++)
        ui.cells(0, SHEET_ROW0 + 1 + e, pad("", 80), COL_DIM, BG);
}

function scene() {
    ui.clear(BG);
    drawToolbar();
    drawGrid();
    for (var i = 0; i < comps.length; i++)
        drawComp(comps[i], i === selComp);
    drawFormula();
    drawSheet();
}

/* ===== 操作 ===== */
function recompute() { ana = analyze(comps); }

function nearestNode(x, y) {
    var c = Math.round((x - GX0) / SP), r = Math.round((y - GY0) / SP);
    if (c < 0 || c >= GC || r < 0 || r >= GR) return -1;
    var px = GX0 + c * SP, py = GY0 + r * SP;
    if (Math.abs(x - px) > 26 || Math.abs(y - py) > 26) return -1;
    return r * 16 + c;
}

function nearestComp(x, y) {
    var best = -1, bd = 33;
    for (var i = 0; i < comps.length; i++) {
        var mx = (nx(comps[i].a) + nx(comps[i].b)) >> 1;
        var my = (ny(comps[i].a) + ny(comps[i].b)) >> 1;
        var d = Math.max(Math.abs(x - mx), Math.abs(y - my));
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

function adjacent(a, b) {
    var dc = Math.abs((a % 16) - (b % 16));
    var dr = Math.abs(((a / 16) | 0) - ((b / 16) | 0));
    return (dc === 1 && dr === 0) || (dc === 0 && dr === 1);
}

function placeComp(a, b) {
    for (var i = 0; i < comps.length; i++) {
        var c = comps[i];
        if ((c.a === a && c.b === b) || (c.a === b && c.b === a)) {
            comps.splice(i, 1);
            break;
        }
    }
    if (mode === "W") {
        comps.push({ id: "", kind: "W", a: a, b: b, expr: "", val: 0 });
    } else {
        seq[mode]++;
        comps.push({ id: mode + seq[mode], kind: mode, a: a, b: b,
                     expr: mode === "R" ? "100" : "5",
                     val: mode === "R" ? 100 : 5 });
        selComp = comps.length - 1;
    }
}

function nodeTap(nn) {
    selComp = -1;
    if (selNode < 0 || selNode === nn) {
        selNode = selNode === nn ? -1 : nn;
        scene();
        return;
    }
    if (!adjacent(selNode, nn)) {
        selNode = nn;
        scene();
        return;
    }
    placeComp(selNode, nn);
    selNode = nn; /* 連続配置: 2 点目が次の始点 */
    recompute();
    save();
    scene();
}

function startEdit(i) {
    editIdx = i;
    editBuf = comps[i].expr;
    selComp = i;
    ui.keyboard(1);
    scene();
}

function commitEdit(ok) {
    if (editIdx < 0) return;
    if (ok && editBuf.length) comps[editIdx].expr = editBuf;
    editIdx = -1;
    editBuf = "";
    ui.keyboard(0);
    recompute();
    save();
    scene();
}

function handleTouch(x, y, kind) {
    if (kind !== 0) return;
    if (editIdx >= 0) commitEdit(true); /* 表計算ライク: 他をタップで確定 */

    if (y < 72) {
        var bi = ((x - 8) / 142) | 0;
        if (bi < 0 || bi >= TOOLS.length) return;
        if (x < 8 + bi * 142 || x > 8 + bi * 142 + 134) return;
        if (TOOLS[bi].m === "CLR") {
            comps = [];
            seq = { R: 0, V: 0 };
            selNode = -1;
            selComp = -1;
            recompute();
            save();
        } else {
            mode = TOOLS[bi].m;
            selNode = -1;
        }
        scene();
        return;
    }

    if (y < 580) {
        var nn = mode === "DEL" ? -1 : nearestNode(x, y);
        if (nn >= 0) { nodeTap(nn); return; }
        var ci = nearestComp(x, y);
        if (ci >= 0) {
            if (mode === "DEL") {
                comps.splice(ci, 1);
                if (selComp === ci) selComp = -1;
                else if (selComp > ci) selComp--;
                recompute();
                save();
            } else {
                selComp = ci;
            }
            scene();
        } else if (selNode >= 0 || selComp >= 0) {
            selNode = -1;
            selComp = -1;
            scene();
        }
        return;
    }

    var idx = ((y / 24) | 0) - SHEET_ROW0 - 1;
    var list = sheetComps();
    if (idx >= 0 && idx < list.length && idx < SHEET_MAX) {
        for (var i = 0; i < comps.length; i++) {
            if (comps[i] === list[idx]) { startEdit(i); return; }
        }
    }
}

function handleKey(k) {
    if (editIdx < 0) return;
    if (k === "\n") { commitEdit(true); return; }
    if (k === "\b") {
        editBuf = editBuf.slice(0, -1);
    } else if (k.charCodeAt(0) === 27) {
        if (k.length > 1) return; /* 矢印等のエスケープ列は無視 */
        commitEdit(false);
        return;
    } else if (k.length === 1) {
        editBuf += k;
    }
    scene();
}

/* ===== SELFTEST (PC run_pc: UI なしのとき) ===== */
function selftest() {
    var fails = 0;
    function ok(cond, name) {
        if (cond) print("ok " + name);
        else { fails++; print("FAIL " + name); }
    }
    function near(x, y) {
        return Math.abs(x - y) <= Math.abs(y) * 1e-9 + 1e-12;
    }
    function lk(v) {
        return function (id) {
            if (v[id] === undefined) throw new Error("未定義: " + id);
            return v[id];
        };
    }
    function n(c, r) { return r * 16 + c; }
    function C(kind, a, b, expr, id) {
        return { kind: kind, a: a, b: b, expr: expr, id: id, val: 0 };
    }

    /* 式評価 */
    ok(near(evalExpr("100", lk({})), 100), "expr num");
    ok(near(evalExpr("10k", lk({})), 10000), "expr 10k");
    ok(near(evalExpr("4.7k", lk({})), 4700), "expr 4.7k");
    ok(near(evalExpr("10m", lk({})), 0.01), "expr 10m");
    ok(near(evalExpr("2*R1+50", lk({ R1: 100 })), 250), "expr ref");
    ok(near(evalExpr("1/(1/R1+1/R2)", lk({ R1: 100, R2: 100 })), 50), "expr par");
    ok(near(evalExpr("-3*-2", lk({})), 6), "expr unary");
    var threw = false;
    try { evalExpr("1+", lk({})); } catch (e) { threw = true; }
    ok(threw, "expr syntax error");

    /* フォーマット */
    ok(fmt(150) === "150", "fmt 150: " + fmt(150));
    ok(fmt(10000) === "10k", "fmt 10k: " + fmt(10000));
    ok(fmt(4700) === "4.7k", "fmt 4.7k: " + fmt(4700));
    ok(fmt(0.0333) === "33.3m", "fmt 33.3m: " + fmt(0.0333));
    ok(fmt(0) === "0", "fmt 0");

    /* 直列ループ: V1 5V + R1 100 + R2 50 */
    var r = analyze([
        C("V", n(0, 0), n(0, 1), "5", "V1"),
        C("R", n(0, 0), n(1, 0), "100", "R1"),
        C("R", n(1, 0), n(1, 1), "50", "R2"),
        C("W", n(1, 1), n(0, 1), "", "")
    ]);
    ok(r.ok, "series ok: " + r.msg);
    ok(near(r.rtot, 150), "series rtot");
    ok(near(r.itot, 5 / 150), "series itot");
    ok(near(r.per.R1.v, 5 * 100 / 150), "series vR1");
    ok(near(r.per.R2.v, 5 * 50 / 150), "series vR2");
    ok(r.fml.indexOf("+") >= 0, "series fml: " + r.fml);

    /* 並列: R1 // R2 (100, 100) */
    r = analyze([
        C("V", n(0, 0), n(0, 1), "5", "V1"),
        C("W", n(0, 0), n(1, 0), "", ""),
        C("W", n(1, 0), n(2, 0), "", ""),
        C("W", n(0, 1), n(1, 1), "", ""),
        C("W", n(1, 1), n(2, 1), "", ""),
        C("R", n(1, 0), n(1, 1), "100", "R1"),
        C("R", n(2, 0), n(2, 1), "100", "R2")
    ]);
    ok(r.ok, "par ok: " + r.msg);
    ok(near(r.rtot, 50), "par rtot");
    ok(near(r.per.R1.i, 0.05), "par iR1");
    ok(near(r.itot, 0.1), "par itot");
    ok(r.fml.indexOf("//") >= 0, "par fml: " + r.fml);

    /* 混合: R1 + (R2 // R3)、式参照 R3 = R2 */
    r = analyze([
        C("V", n(0, 0), n(0, 1), "6", "V1"),
        C("R", n(0, 0), n(1, 0), "100", "R1"),
        C("R", n(1, 0), n(1, 1), "200", "R2"),
        C("W", n(1, 0), n(2, 0), "", ""),
        C("R", n(2, 0), n(2, 1), "R2", "R3"),
        C("W", n(2, 1), n(1, 1), "", ""),
        C("W", n(1, 1), n(0, 1), "", "")
    ]);
    ok(r.ok, "mixed ok: " + r.msg);
    ok(near(r.rtot, 200), "mixed rtot: " + r.rtot);
    ok(near(r.per.R1.v, 3), "mixed vR1");
    ok(near(r.per.R2.i, 0.015), "mixed iR2");
    ok(r.fml.indexOf("//") >= 0 && r.fml.indexOf("+") >= 0,
       "mixed fml: " + r.fml);

    /* 短絡された抵抗: I = 0 で除外され Rtot は残り */
    r = analyze([
        C("V", n(0, 0), n(0, 1), "5", "V1"),
        C("R", n(0, 0), n(1, 0), "100", "R1"),
        C("W", n(0, 0), n(1, 0), "", ""),
        C("R", n(1, 0), n(1, 1), "50", "R2"),
        C("W", n(1, 1), n(0, 1), "", "")
    ]);
    ok(r.ok, "short ok: " + r.msg);
    ok(near(r.rtot, 50), "short rtot: " + r.rtot);
    ok(r.per.R1.i === 0, "short iR1=0");

    /* ぶら下がり抵抗: I = 0 */
    r = analyze([
        C("V", n(0, 0), n(0, 1), "5", "V1"),
        C("R", n(0, 0), n(1, 0), "100", "R1"),
        C("W", n(1, 0), n(1, 1), "", ""),
        C("W", n(1, 1), n(0, 1), "", ""),
        C("R", n(1, 0), n(2, 0), "100", "R9")
    ]);
    ok(r.ok, "dangle ok: " + r.msg);
    ok(near(r.rtot, 100), "dangle rtot");
    ok(r.per.R9.i === 0, "dangle iR9=0");

    /* 開回路 */
    r = analyze([
        C("V", n(0, 0), n(0, 1), "5", "V1"),
        C("R", n(0, 0), n(1, 0), "100", "R1")
    ]);
    ok(!r.ok, "open detected: " + r.msg);

    /* ブリッジ (非直並列) */
    r = analyze([
        C("V", n(0, 0), n(3, 0), "5", "V1"),
        C("R", n(0, 0), n(1, 0), "100", "R1"),
        C("R", n(0, 0), n(2, 0), "100", "R2"),
        C("R", n(1, 0), n(3, 0), "100", "R3"),
        C("R", n(2, 0), n(3, 0), "100", "R4"),
        C("R", n(1, 0), n(2, 0), "100", "R5")
    ]);
    ok(!r.ok && r.msg.indexOf("直並列") >= 0, "bridge detected: " + r.msg);

    /* 循環参照 */
    r = analyze([
        C("V", n(0, 0), n(0, 1), "5", "V1"),
        C("R", n(0, 0), n(1, 0), "R2*2", "R1"),
        C("R", n(1, 0), n(1, 1), "R1", "R2"),
        C("W", n(1, 1), n(0, 1), "", "")
    ]);
    ok(!r.ok && r.msg.indexOf("循環") >= 0, "cycle detected: " + r.msg);

    if (fails) throw new Error("circuit selftest: " + fails + " failed");
    print("circuit selftest: ALL PASS");
}

/* ===== 起動 ===== */
if (HAS_UI) {
    load();
    recompute();
    scene();
    ui.onTouch(handleTouch);
    ui.onKey(handleKey);
    sys.onForeground(function () {
        selNode = -1;
        editIdx = -1;
        editBuf = "";
        ui.keyboard(0);
        recompute();
        scene();
    });
} else {
    selftest();
}
