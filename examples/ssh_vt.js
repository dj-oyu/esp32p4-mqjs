/* Tab5 SSH ターミナルエミュレータ (Phase T3 + W3): mquickjs 製 VT100、
 * マルチセッション対応。
 *
 * 等幅セルグリッドを持ち、サーバから来る ANSI/VT100 エスケープ
 * (カーソル移動・消去・SGR 16 色・スクロール領域) を解釈して、C 側の
 * ui.cells (グリフ直接ブリット) と ui.scroll (バッファスクロール) で
 * 高速に描画する。ls --color / vi / top が軽く動く。
 *
 * W3: ssh.* はハンドル式 (connect が id を返す)。セッションは最大 3 本
 * 同時にキープでき、端末状態は makeTerm() ファクトリでセッション毎に
 * 独立。画面最上段の 1 行はタブバー: タップで切替、[+] で新規接続。
 * 非アクティブな端末はモデルだけ更新し (描画ゲート)、切替時に全再描画。
 *
 * ホストは store("ssh_hosts") にローカル永続 (W2)。切断するとそのタブが
 * 消え、最後のセッションが終わるとホスト一覧に戻る。
 *
 * 検証モード (どちらも committed 版は false):
 *   SELFTEST=true … PC/実機でパーサを走らせ grid を print ダンプ
 *   REPORT=true   … 実機で色付きデモを画面に流し続ける (SSH 不要・目視用) */
"use strict";
sys.setAppName("ssh_vt");

var HOST = "192.168.1.10";
var PORT = 22;
var USER = "user";
var PASS = "";

/* 検証フラグ。コメントは必ず 1 行に収める (フラグを sed で書き換えて
   push するため、複数行コメントだと注入で構文が壊れる)。 */
var SELFTEST = false; /* PC: DEMO_SCRIPT を流して grid を print ダンプ */
var REPORT = false;   /* 実機: DEMO_SCRIPT を画面描画し続ける SSH 無しデモ (目視確認用) */

var DEMO_SCRIPT =
    "\x1b[2J\x1b[H" +
    "\x1b[1;34mTab5 VT100\x1b[0m emulator\r\n" +
    "\x1b[32m$ ls --color\x1b[0m\r\n" +
    "\x1b[34mdir1\x1b[0m \x1b[1;32mexec\x1b[0m file.txt\r\n" +
    "move:\x1b[10Cgap10\r\n" +
    "\x1b[7;1Habs7-1\r\n" +
    "\x1b[7minverse\x1b[0m bar\r\n" +
    "tab\there";

/* ---- 画面メトリクス (等幅セルグリッド) ----
   桁数/行数は画面サイズとセル寸法から導出する (ハードコードしない)。
   最上段 TAB_ROWS 行はタブバーに割り、端末グリッドはその下に置く。 */
var sz = ui.size();
var W = sz[0] || 720, H = sz[1] || 1192;
var KB_H = 400;                     /* 常時表示キーボードの高さ */
var VIEW_H = H - KB_H;
var cell = ui.cellSize();           /* HackGen 等幅: [9, 24] */
var CW = cell[0] || 9;
var LH = cell[1] || 24;
var COLS = (W / CW) | 0;            /* 720/9 = 80 */
var GRID_ROWS = (VIEW_H / LH) | 0;  /* タブバー込みの総行数 */
var TAB_ROWS = 1;                   /* タブバーの行数 */
var ROWS = GRID_ROWS - TAB_ROWS;    /* 端末の行数 (= pty rows) */
if (COLS < 1) COLS = 1;
if (ROWS < 1) ROWS = 1;

/* ---- 配色 (コンソールと同じ 16 色パレット、暗背景向け) ---- */
var BG = 0x0B0E11;
var FG = 0xC9D1D9;
var CURSOR = 0x4FC3F7;
var TAB_BG = 0x1A222C;
var TAB_FG = 0x8B98A5;
var TAB_ACT_BG = 0x2E6BD6;
var TAB_ACT_FG = 0xFFFFFF;
var PALETTE = [
    0x55606B, 0xE05A4E, 0x2ECC71, 0xFFD479,  /* 0-3  blk red grn yel */
    0x4FC3F7, 0xC678DD, 0x56B6C2, 0xC9D1D9,  /* 4-7  blu mag cyn wht */
    0x8B98A5, 0xFF6B5E, 0x4AE38A, 0xFFE08A,  /* 8-15 bright          */
    0x6FD3FF, 0xD898E8, 0x7FD8E0, 0xFFFFFF
];

function colorOf(idx, deflt) {
    return idx === 0 ? deflt : PALETTE[idx - 1];
}

var SP = " ".repeat(COLS);

/* ---- 端末 1 本ぶんの状態 + パーサ + 描画 (W3: セッション毎に独立) ----
 * 描画はアクティブな端末だけが行う (self.act ゲート)。非アクティブ中も
 * モデルと dirty フラグは更新され、切替時の markAll + flush で追い付く。
 * 画面上の行 = 端末行 + TAB_ROWS (タブバーの下)。 */
function makeTerm() {
    function newRow() {
        return { ch: SP, fg: new Uint8Array(COLS), bg: new Uint8Array(COLS) };
    }
    var rows = new Array(ROWS);
    var i;
    for (i = 0; i < ROWS; i++)
        rows[i] = newRow();

    var cx = 0, cy = 0;                 /* カーソル */
    var savedCx = 0, savedCy = 0;
    var scrollTop = 0, scrollBot = ROWS - 1;
    var curFg = 0, curBg = 0, bold = false, inverse = false;
    var prevCurRow = 0;

    /* dirty[r] = 要再描画。dirtySeq[r] = 最後にダーティ化した順序番号。 */
    var dirty = new Array(ROWS);
    var dirtySeq = new Array(ROWS);
    var seq = 0;
    for (i = 0; i < ROWS; i++) {
        dirty[i] = true;
        dirtySeq[i] = i;
    }
    seq = ROWS;
    function markDirty(r) {
        if (r >= 0 && r < ROWS) {
            dirty[r] = true;
            dirtySeq[r] = ++seq;
        }
    }
    function markAll() {
        for (var r = 0; r < ROWS; r++)
            markDirty(r);
    }

    function setCell(r, c, chr) {
        var row = rows[r];
        row.ch = row.ch.slice(0, c) + chr + row.ch.slice(c + 1);
        var f = curFg, b = curBg;
        if (bold && f >= 1 && f <= 8)
            f += 8;
        if (inverse) {
            var t = f === 0 ? 8 : f;
            f = b === 0 ? 1 : b;
            b = t;
        }
        row.fg[c] = f;
        row.bg[c] = b;
        markDirty(r);
    }

    function blankCell(r, c) {
        var row = rows[r];
        row.ch = row.ch.slice(0, c) + " " + row.ch.slice(c + 1);
        row.fg[c] = 0;
        row.bg[c] = inverse ? (curFg === 0 ? 8 : curFg) : 0;
        markDirty(r);
    }

    /* スクロール領域を 1 行ずらす。アクティブ時はバッファも ui.scroll で
       同じ向きにずらす (タブバーぶん +TAB_ROWS のオフセット)。 */
    function scrollUp() {
        var first = rows[scrollTop];
        for (var r = scrollTop; r < scrollBot; r++) {
            rows[r] = rows[r + 1];
            dirty[r] = dirty[r + 1];
            dirtySeq[r] = dirtySeq[r + 1];
        }
        first.ch = SP;
        first.fg = new Uint8Array(COLS);
        first.bg = new Uint8Array(COLS);
        rows[scrollBot] = first;
        if (self.act) {
            dirty[scrollBot] = false;
            dirtySeq[scrollBot] = ++seq;
            ui.scroll(scrollTop + TAB_ROWS, scrollBot + TAB_ROWS, 1, BG);
        } else {
            /* 画面には反映していない: 切替時に全再描画されるので
               この行も dirty のままにしておく */
            markDirty(scrollBot);
        }
    }

    function scrollDown() {
        var last = rows[scrollBot];
        for (var r = scrollBot; r > scrollTop; r--) {
            rows[r] = rows[r - 1];
            dirty[r] = dirty[r - 1];
            dirtySeq[r] = dirtySeq[r - 1];
        }
        last.ch = SP;
        last.fg = new Uint8Array(COLS);
        last.bg = new Uint8Array(COLS);
        rows[scrollTop] = last;
        if (self.act) {
            dirty[scrollTop] = false;
            dirtySeq[scrollTop] = ++seq;
            ui.scroll(scrollTop + TAB_ROWS, scrollBot + TAB_ROWS, -1, BG);
        } else {
            markDirty(scrollTop);
        }
    }

    function lineFeed() {
        if (cy === scrollBot)
            scrollUp();
        else if (cy < ROWS - 1)
            cy++;
    }

    /* ---- エスケープ状態機械 ---- */
    var ST_GROUND = 0, ST_ESC = 1, ST_CSI = 2, ST_OSC = 3, ST_CHARSET = 4;
    var state = ST_GROUND;
    var csiParams = "";
    var csiPriv = false;

    function params() {
        var out = [];
        var parts = csiParams.split(";");
        for (var k = 0; k < parts.length; k++) {
            var v = parseInt(parts[k], 10);
            out.push(isNaN(v) ? 0 : v);
        }
        return out;
    }
    function param0(p, deflt) {
        return (p.length === 0 || p[0] === 0) ? deflt : p[0];
    }

    function clamp(v, lo, hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    function eraseInLine(mode) {
        var c;
        if (mode === 1) {
            for (c = 0; c <= cx && c < COLS; c++)
                blankCell(cy, c);
        } else if (mode === 2) {
            for (c = 0; c < COLS; c++)
                blankCell(cy, c);
        } else {
            for (c = cx; c < COLS; c++)
                blankCell(cy, c);
        }
    }

    function eraseInDisplay(mode) {
        var r, c;
        if (mode === 1) {
            for (r = 0; r < cy; r++)
                for (c = 0; c < COLS; c++)
                    blankCell(r, c);
            eraseInLine(1);
        } else if (mode === 2 || mode === 3) {
            for (r = 0; r < ROWS; r++)
                for (c = 0; c < COLS; c++)
                    blankCell(r, c);
        } else {
            eraseInLine(0);
            for (r = cy + 1; r < ROWS; r++)
                for (c = 0; c < COLS; c++)
                    blankCell(r, c);
        }
    }

    function applySGR(p) {
        if (p.length === 0)
            p = [0];
        for (var k = 0; k < p.length; k++) {
            var v = p[k];
            if (v === 0) {
                curFg = 0; curBg = 0; bold = false; inverse = false;
            } else if (v === 1) {
                bold = true;
            } else if (v === 22) {
                bold = false;
            } else if (v === 7) {
                inverse = true;
            } else if (v === 27) {
                inverse = false;
            } else if (v >= 30 && v <= 37) {
                curFg = v - 30 + 1;
            } else if (v === 39) {
                curFg = 0;
            } else if (v >= 40 && v <= 47) {
                curBg = v - 40 + 1;
            } else if (v === 49) {
                curBg = 0;
            } else if (v >= 90 && v <= 97) {
                curFg = v - 90 + 8 + 1;
            } else if (v >= 100 && v <= 107) {
                curBg = v - 100 + 8 + 1;
            } else if (v === 38 || v === 48) {
                if (p[k + 1] === 5) {
                    var idx = p[k + 2];
                    var col = idx < 16 ? idx + 1 : (idx < 244 ? 8 : 16);
                    if (v === 38) curFg = col; else curBg = col;
                    k += 2;
                } else if (p[k + 1] === 2) {
                    k += 4;
                }
            }
        }
    }

    function csiDispatch(fin) {
        var p = params();
        var n, r, c;
        if (fin === "A") { cy = clamp(cy - param0(p, 1), 0, ROWS - 1); }
        else if (fin === "B") { cy = clamp(cy + param0(p, 1), 0, ROWS - 1); }
        else if (fin === "C") { cx = clamp(cx + param0(p, 1), 0, COLS - 1); }
        else if (fin === "D") { cx = clamp(cx - param0(p, 1), 0, COLS - 1); }
        else if (fin === "G" || fin === "`") { cx = clamp(param0(p, 1) - 1, 0, COLS - 1); }
        else if (fin === "d") { cy = clamp(param0(p, 1) - 1, 0, ROWS - 1); }
        else if (fin === "H" || fin === "f") {
            cy = clamp((p[0] || 1) - 1, 0, ROWS - 1);
            cx = clamp((p[1] || 1) - 1, 0, COLS - 1);
        }
        else if (fin === "J") { eraseInDisplay(p[0] || 0); }
        else if (fin === "K") { eraseInLine(p[0] || 0); }
        else if (fin === "m") { applySGR(p); }
        else if (fin === "L") { /* 行挿入 */
            n = param0(p, 1);
            for (var li = 0; li < n; li++) {
                if (cy <= scrollBot) {
                    var sb = scrollBot, sct = cy;
                    var last = rows[sb];
                    for (r = sb; r > sct; r--) rows[r] = rows[r - 1];
                    last.ch = SP; last.fg = new Uint8Array(COLS); last.bg = new Uint8Array(COLS);
                    rows[sct] = last;
                }
            }
            for (r = cy; r <= scrollBot; r++) markDirty(r);
        }
        else if (fin === "M") { /* 行削除 */
            n = param0(p, 1);
            for (var di = 0; di < n; di++) {
                if (cy <= scrollBot) {
                    var first = rows[cy];
                    for (r = cy; r < scrollBot; r++) rows[r] = rows[r + 1];
                    first.ch = SP; first.fg = new Uint8Array(COLS); first.bg = new Uint8Array(COLS);
                    rows[scrollBot] = first;
                }
            }
            for (r = cy; r <= scrollBot; r++) markDirty(r);
        }
        else if (fin === "P") { /* 文字削除 (左詰め) */
            n = param0(p, 1);
            var row = rows[cy];
            var s = row.ch.slice(0, cx) + row.ch.slice(cx + n) + " ".repeat(n);
            row.ch = s.slice(0, COLS);
            for (c = cx; c < COLS; c++) {
                row.fg[c] = c + n < COLS ? row.fg[c + n] : 0;
                row.bg[c] = c + n < COLS ? row.bg[c + n] : 0;
            }
            markDirty(cy);
        }
        else if (fin === "@") { /* 空白挿入 */
            n = param0(p, 1);
            var row2 = rows[cy];
            var s2 = row2.ch.slice(0, cx) + " ".repeat(n) + row2.ch.slice(cx);
            row2.ch = s2.slice(0, COLS);
            for (c = COLS - 1; c >= cx; c--) {
                row2.fg[c] = c - n >= cx ? row2.fg[c - n] : 0;
                row2.bg[c] = c - n >= cx ? row2.bg[c - n] : 0;
            }
            markDirty(cy);
        }
        else if (fin === "r") { /* スクロール領域 DECSTBM */
            scrollTop = clamp((p[0] || 1) - 1, 0, ROWS - 1);
            scrollBot = clamp((p[1] || ROWS) - 1, 0, ROWS - 1);
            if (scrollBot <= scrollTop) { scrollTop = 0; scrollBot = ROWS - 1; }
            cx = 0; cy = scrollTop;
        }
        else if (fin === "s") { savedCx = cx; savedCy = cy; }
        else if (fin === "u") { cx = savedCx; cy = savedCy; }
    }

    function feed(s) {
        for (var i2 = 0; i2 < s.length; i2++) {
            var ch = s[i2];
            var code = s.charCodeAt(i2);

            if (state === ST_OSC) {
                if (code === 7) state = ST_GROUND;
                else if (code === 27) state = ST_ESC;
                continue;
            }
            if (state === ST_CHARSET) {
                state = ST_GROUND;
                continue;
            }
            if (state === ST_ESC) {
                if (ch === "[") { state = ST_CSI; csiParams = ""; csiPriv = false; }
                else if (ch === "]") { state = ST_OSC; }
                else if (ch === "(" || ch === ")") { state = ST_CHARSET; }
                else if (ch === "M") {
                    if (cy === scrollTop) scrollDown(); else if (cy > 0) cy--;
                    state = ST_GROUND;
                }
                else if (ch === "D") { lineFeed(); state = ST_GROUND; }
                else if (ch === "E") { cx = 0; lineFeed(); state = ST_GROUND; }
                else if (ch === "7") { savedCx = cx; savedCy = cy; state = ST_GROUND; }
                else if (ch === "8") { cx = savedCx; cy = savedCy; state = ST_GROUND; }
                else if (ch === "c") { resetTerm(); state = ST_GROUND; }
                else if (ch === "\\") { state = ST_GROUND; }
                else { state = ST_GROUND; }
                continue;
            }
            if (state === ST_CSI) {
                if (ch === "?") { csiPriv = true; }
                else if ((code >= 0x30 && code <= 0x39) || ch === ";") { csiParams += ch; }
                else if (code >= 0x40 && code <= 0x7E) {
                    csiDispatch(ch);
                    state = ST_GROUND;
                }
                continue;
            }

            if (code === 27) { state = ST_ESC; }
            else if (code === 13) { cx = 0; }
            else if (code === 10) { lineFeed(); }
            else if (code === 8) { if (cx > 0) cx--; }
            else if (code === 9) {
                cx = clamp((cx + 8) & ~7, 0, COLS - 1);
            }
            else if (code === 7) { /* BEL */ }
            else if (code < 32) { /* C0 */ }
            else {
                if (cx >= COLS) { cx = 0; lineFeed(); }
                setCell(cy, cx, ch);
                cx++;
            }
        }
        markDirty(prevCurRow);
        markDirty(cy);
        prevCurRow = cy;
    }

    function resetTerm() {
        for (var r = 0; r < ROWS; r++) {
            rows[r].ch = SP;
            rows[r].fg = new Uint8Array(COLS);
            rows[r].bg = new Uint8Array(COLS);
        }
        cx = 0; cy = 0; scrollTop = 0; scrollBot = ROWS - 1;
        curFg = 0; curBg = 0; bold = false; inverse = false;
        markAll();
    }

    /* ---- 描画 (1 行を同色ランに分けて ui.cells で出す) ---- */
    function drawRow(r) {
        var row = rows[r];
        var cmds = 0;
        var c = 0;
        while (c < COLS) {
            var f = row.fg[c], b = row.bg[c];
            var c2 = c + 1;
            while (c2 < COLS && row.fg[c2] === f && row.bg[c2] === b)
                c2++;
            ui.cells(c, r + TAB_ROWS, row.ch.slice(c, c2),
                     colorOf(f, FG), colorOf(b, BG));
            cmds++;
            c = c2;
        }
        return cmds;
    }

    var FLUSH_BUDGET = 90;
    function flush() {
        if (!self.act)
            return;
        var budget = FLUSH_BUDGET;
        while (budget > 0) {
            var best = -1, bestSeq = -1;
            for (var r = 0; r < ROWS; r++) {
                if (dirty[r] && dirtySeq[r] > bestSeq) {
                    best = r;
                    bestSeq = dirtySeq[r];
                }
            }
            if (best < 0)
                break;
            budget -= drawRow(best);
            dirty[best] = false;
        }
        /* カーソル (アンダーライン) — 行描画の上に重ねる */
        if (cy < ROWS && cx < COLS)
            ui.rect(cx * CW, (cy + TAB_ROWS) * LH + LH - 3, CW, 2, CURSOR);
    }

    var self = {
        act: false,
        feed: feed,
        flush: flush,
        markAll: markAll,
        reset: resetTerm,
        rows: rows,
        cursor: function () { return [cx, cy]; }
    };
    return self;
}

/* ---- セルフテスト用ヘルパ (パーサ検証、W1 と出力互換) ---- */
function gridLines(t) {
    var out = [];
    for (var r = 0; r < ROWS; r++) {
        var line = t.rows[r].ch;
        var e = line.length;
        while (e > 0 && line[e - 1] === " ") e--;
        if (e > 0)
            out.push(("0" + r).slice(-2) + "|" + line.slice(0, e));
    }
    return out;
}

if (SELFTEST) {
    var t = makeTerm();
    t.feed(DEMO_SCRIPT);
    var lines = gridLines(t);
    for (var li = 0; li < lines.length; li++)
        print(lines[li]);
    var cur = t.cursor();
    print("END cursor=" + cur[0] + "," + cur[1] + " cols_vis=" + COLS +
          " rows_vis=" + ROWS + " cw=" + CW + " lh=" + LH);
} else if (REPORT) {
    var rt = makeTerm();
    rt.act = true;
    ui.clear(BG);
    rt.feed(DEMO_SCRIPT);
    rt.feed("\r\n");
    setInterval(function () { rt.flush(); }, 25);
    var tick = 0;
    setInterval(function () {
        tick++;
        var col = 31 + (tick % 7);
        rt.feed("\x1b[" + col + "mline " + tick +
                "\x1b[0m  scroll/cursor/SGR ok\r\n");
    }, 500);
} else {
    /* ---- SSH クライアント (W2 ホストページ + W3 マルチセッション) ---- */
    var inForm = false;
    var HOSTS_KEY = "ssh_hosts";

    /* sessions[i] = { id, term, label } — 並び順がタブ順 */
    var sessions = [];
    var actIdx = -1;          /* sessions のアクティブ index */
    var MAX_SESS = 3;         /* == C 側 SSHC_MAX_SESSIONS */
    var tabHit = [];          /* タブバーのタップ判定 [{x0,x1,idx}] */

    var loadHosts = function () {
        var s = store.get(HOSTS_KEY);
        if (!s)
            return [];
        try {
            var a = JSON.parse(s);
            return (a && a.length !== undefined) ? a : [];
        } catch (e) {
            return [];
        }
    };
    var saveHosts = function (hosts) {
        store.set(HOSTS_KEY, JSON.stringify(hosts));
    };
    var unwind = function () {
        while (ui.back()) {}
    };
    var hostLabel = function (e) {
        return e.user + "@" + e.host + ":" + e.port;
    };

    /* ---- タブバー (キャンバス最上段、ui.cells 直描き) ---- */
    var drawTabs = function () {
        tabHit = [];
        /* 行クリア */
        ui.cells(0, 0, SP, TAB_FG, TAB_BG);
        var c = 0;
        for (var i = 0; i < sessions.length; i++) {
            var label = " " + (i + 1) + ":" + sessions[i].label + " ";
            if (label.length > 22)
                label = label.slice(0, 21) + "… ";
            if (c + label.length >= COLS - 4)
                break;
            var act = (i === actIdx);
            ui.cells(c, 0, label, act ? TAB_ACT_FG : TAB_FG,
                     act ? TAB_ACT_BG : TAB_BG);
            tabHit.push({ x0: c * CW, x1: (c + label.length) * CW, idx: i });
            c += label.length;
        }
        if (sessions.length < MAX_SESS) {
            var plus = " + ";
            ui.cells(c, 0, plus, TAB_ACT_FG, 0x256B45);
            tabHit.push({ x0: c * CW, x1: (c + plus.length) * CW, idx: -1 });
        }
    };

    var switchTo = function (i) {
        if (i < 0 || i >= sessions.length)
            return;
        if (actIdx >= 0 && actIdx < sessions.length)
            sessions[actIdx].term.act = false;
        actIdx = i;
        var t = sessions[i].term;
        t.act = true;
        t.markAll();   /* 切替: 全行を flush 対象に */
        drawTabs();
    };

    ui.onKey(function (k) {
        if (actIdx < 0)
            return;
        var id = sessions[actIdx].id;
        if (k === "\n")
            ssh.write(id, "\r");
        else if (k === "\b")
            ssh.write(id, "\x7f");
        else
            ssh.write(id, k);
    });

    /* タブバーのタップ: 切替 / [+] = ホストページ。それ以外のタップは
       キーボード呼び戻し (ページ前面中は inForm で遮断)。 */
    ui.onTouch(function (x, y, kind) {
        if (inForm || kind !== 0)
            return;
        if (y < TAB_ROWS * LH + 8) {
            for (var i = 0; i < tabHit.length; i++) {
                if (x >= tabHit[i].x0 && x < tabHit[i].x1) {
                    if (tabHit[i].idx < 0)
                        hostsPage(null);
                    else
                        switchTo(tabHit[i].idx);
                    return;
                }
            }
            return;
        }
        ui.keyboard(1);
    });

    setInterval(function () {
        if (actIdx >= 0 && actIdx < sessions.length)
            sessions[actIdx].term.flush();
    }, 25); /* ~40fps でアクティブ端末のダーティ行を消化 */

    /* セッションを開く。成功で端末ビューへ。 */
    var startSession = function (e) {
        if (sessions.length >= MAX_SESS) {
            hostsPage("セッション上限 (" + MAX_SESS + " 本) です");
            return;
        }
        var id;
        try {
            id = ssh.connect(e.host, e.port, e.user, e.pass, COLS, ROWS);
        } catch (err) {
            hostsPage("接続できない: " + err.message);
            return;
        }
        var term = makeTerm();
        var entry = { id: id, term: term, label: e.user + "@" + e.host };
        sessions.push(entry);

        ssh.onData(id, function (chunk) {
            term.feed(chunk);
        });
        ssh.onClose(id, function (reason) {
            /* このセッションのタブを畳む */
            var i = sessions.indexOf(entry);
            if (i >= 0)
                sessions.splice(i, 1);
            if (!sessions.length) {
                actIdx = -1;
                ui.keyboard(0);
                hostsPage("切断: " + entry.label + " (" + reason + ")");
                return;
            }
            if (actIdx >= sessions.length || i === actIdx)
                switchTo(i < sessions.length ? i : sessions.length - 1);
            else if (i < actIdx)
                actIdx--; /* 並びが詰まった: index を追従 */
            drawTabs();
        });

        inForm = false;
        unwind();
        ui.clear(BG);
        ui.keyboard(1);
        switchTo(sessions.length - 1);
    };

    /* ---- W2: ホストページ / フォーム (store 永続) ---- */
    var connectForm = function (preset, editIdx) {
        inForm = true;
        var s = ui.screen(editIdx >= 0 ? "ホストを編集" : "新規接続");
        var fh = s.field("Host");
        fh.setText(preset.host);
        var fp = s.field("Port");
        fp.setText("" + preset.port);
        var fu = s.field("User");
        fu.setText(preset.user);
        var fw = s.field("Password", { secret: true });
        fw.setText(preset.pass || "");
        var sv = s.toggle("この接続先を保存", 1);
        var mk = function () {
            var p = parseInt(fp.value(), 10);
            return { host: fh.value(), port: isNaN(p) ? 22 : p,
                     user: fu.value(), pass: fw.value() };
        };
        var put = function (e) {
            if (!sv.value())
                return;
            var hosts = loadHosts();
            if (editIdx >= 0 && editIdx < hosts.length)
                hosts[editIdx] = e;
            else
                hosts.push(e);
            saveHosts(hosts);
        };
        s.button("接続", function () {
            var e = mk();
            put(e);
            startSession(e);
        });
        if (editIdx >= 0) {
            s.button("保存して一覧へ", function () {
                put(mk());
                unwind();
                hostsPage("保存しました");
            });
        }
        s.button("キャンセル", ui.back);
    };

    var hostActions = function (idx) {
        var hosts = loadHosts();
        if (idx >= hosts.length)
            return;
        var e = hosts[idx];
        var s = ui.screen(hostLabel(e));
        s.button("接続", function () { startSession(e); });
        s.button("編集", function () { connectForm(e, idx); });
        s.button("削除", function () {
            var h2 = loadHosts();
            h2.splice(idx, 1);
            saveHosts(h2);
            unwind();
            hostsPage("削除: " + hostLabel(e));
        });
        s.button("戻る", ui.back);
    };

    var hostsPage = function (note) {
        inForm = true;
        var s = ui.screen("SSH ホスト");
        if (note)
            s.label(note);
        if (sessions.length)
            s.label("接続中: " + sessions.length + " / " + MAX_SESS + " 本");
        var hosts = loadHosts();
        if (hosts.length) {
            var l = s.list();
            var i;
            for (i = 0; i < hosts.length; i++) {
                (function (idx, e) {
                    l.add(hostLabel(e), function () { hostActions(idx); });
                })(i, hosts[i]);
            }
        } else {
            s.label("保存済みホストはありません");
        }
        s.button("+ 新規接続", function () {
            connectForm({ host: HOST, port: PORT, user: USER, pass: "" }, -1);
        });
        if (sessions.length) {
            s.button("端末へ戻る", function () {
                inForm = false;
                unwind();
                /* ページに隠れている間の取りこぼしを念のため埋める */
                if (actIdx >= 0)
                    sessions[actIdx].term.markAll();
                drawTabs();
                ui.keyboard(1);
            });
        }
    };

    /* ---- P4b ライフサイクル: 画面は切替時に C 側で全破棄されるので、
       復帰時にいまのモードを描き直す。モデル (sessions / 端末グリッド /
       hosts) は背景でも生きていて受信は continue するから、端末はフル
       再描画 + flush で追い付き、ページは一覧から再入する (フォーム
       入力中の内容だけは戻らない — モデルではないので)。 */
    sys.onForeground(function () {
        if (!inForm && actIdx >= 0 && actIdx < sessions.length) {
            ui.clear(BG);
            sessions[actIdx].term.markAll(); /* flush 間隔が描き直す */
            drawTabs();
            ui.keyboard(1);
        } else {
            inForm = true;
            hostsPage(sessions.length ? "接続は維持されています" : null);
        }
    });

    hostsPage(null);
}
