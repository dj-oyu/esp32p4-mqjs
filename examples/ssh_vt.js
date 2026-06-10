/* Tab5 SSH ターミナルエミュレータ (Phase T2): mquickjs 製 VT100。
 *
 * 80x24 のセルグリッドを持ち、サーバから来る ANSI/VT100 エスケープ
 * (カーソル移動・消去・SGR 16 色・スクロール領域) を解釈して固定
 * グリッドに描画する。これで ls --color / vi / top がそれなりに見える。
 *
 * 接続先は下の HOST/USER/PASS を自分の環境に書き換える。
 * pty は組込み wolfSSH の都合で 80x24 固定。Tab5 実機のフォントは
 * 1 桁 16px なので 720px に約 45 桁しか入らず、右側 (46-80 桁) は
 * クリップされる。80 桁フルに見せるには横画面化か小フォントが要る (T3)。
 *
 * 検証モード (どちらも committed 版は false):
 *   SELFTEST=true … PC/実機でパーサを走らせ grid を print ダンプ
 *                   (実機なら print は COM8 シリアルに出る → 客観検証)
 *   REPORT=true   … 実機で色付きデモを画面に流し続ける (SSH 不要・目視用) */
"use strict";

var HOST = "192.168.1.10";
var PORT = 22;
var USER = "user";
var PASS = "password";

/* 検証フラグ。コメントは必ず 1 行に収める (フラグを sed で書き換えて
   push するため、複数行コメントだと注入で構文が壊れる)。 */
var SELFTEST = false; /* PC: DEMO_SCRIPT を流して grid を print ダンプ */
var REPORT = false;   /* 実機: DEMO_SCRIPT を画面描画し続ける SSH 無しデモ (目視確認用) */

/* パーサ/描画を一通り叩く台本 (clear/home, SGR色, bold, カーソル前進,
   絶対位置, inverse, autowrap, tab)。SELFTEST と REPORT で共有する。 */
var DEMO_SCRIPT =
    "\x1b[2J\x1b[H" +
    "\x1b[1;34mTab5 VT100\x1b[0m emulator\r\n" +
    "\x1b[32m$ ls --color\x1b[0m\r\n" +
    "\x1b[34mdir1\x1b[0m \x1b[1;32mexec\x1b[0m file.txt\r\n" +
    "move:\x1b[10Cgap10\r\n" +
    "\x1b[7;1Habs7-1\r\n" +
    "\x1b[7minverse\x1b[0m bar\r\n" +
    "tab\there";

/* ---- 画面メトリクス (等幅セルグリッド) ---- */
var COLS = 80, ROWS = 24;          /* pty サイズ (固定) */
var sz = ui.size();
var W = sz[0] || 720, H = sz[1] || 1192;
var KB_H = 400;
var VIEW_H = H - KB_H;
/* ui.cells が使う等幅フォントのセル寸法。HackGen 9x24 なので 720/9 = 80 桁
   ぴったり収まる (もうクリップしない)。 */
var cell = ui.cellSize();
var CW = cell[0] || 9;
var LH = cell[1] || 24;
var COLS_VIS = Math.min(COLS, (W / CW) | 0);
var ROWS_VIS = Math.min(ROWS, (VIEW_H / LH) | 0);

/* ---- 配色 (コンソールと同じ 16 色パレット、暗背景向け) ---- */
var BG = 0x0B0E11;
var FG = 0xC9D1D9;
var CURSOR = 0x4FC3F7;
var PALETTE = [
    0x55606B, 0xE05A4E, 0x2ECC71, 0xFFD479,  /* 0-3  blk red grn yel */
    0x4FC3F7, 0xC678DD, 0x56B6C2, 0xC9D1D9,  /* 4-7  blu mag cyn wht */
    0x8B98A5, 0xFF6B5E, 0x4AE38A, 0xFFE08A,  /* 8-15 bright          */
    0x6FD3FF, 0xD898E8, 0x7FD8E0, 0xFFFFFF
];

/* セルの色は 0=デフォルト、1..16 = PALETTE[idx-1]。
 * Uint8Array はゼロ初期化されるので 0=default が自然に効く。 */
function colorOf(idx, deflt) {
    return idx === 0 ? deflt : PALETTE[idx - 1];
}

/* ---- グリッド ---- */
var SP = " ".repeat(COLS);
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

/* dirty[r] = 要再描画。dirtySeq[r] = 最後にダーティ化した順序番号。
   描画予算が足りないとき、新しくダーティになった行から先に描く
   (スクロールなら最新の下部行、編集ならカーソル周辺が優先される)。 */
var dirty = new Array(ROWS);
var dirtySeq = new Array(ROWS);
var seq = 0;
for (i = 0; i < ROWS; i++) {
    dirty[i] = true;
    dirtySeq[i] = i; /* 初回は上から下へ */
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

/* 行 r の桁 c に 1 文字を書く (色は SGR 状態を反映) */
function setCell(r, c, chr) {
    var row = rows[r];
    row.ch = row.ch.slice(0, c) + chr + row.ch.slice(c + 1);
    var f = curFg, b = curBg;
    if (bold && f >= 1 && f <= 8)
        f += 8;
    if (inverse) {
        var t = f === 0 ? 8 : f; /* default fg -> 白系 */
        f = b === 0 ? 1 : b;     /* default bg -> 暗系 */
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

/* スクロール領域 [scrollTop, scrollBot] を 1 行上へ。
   モデル(rows)と表示バッファ(ui.scroll)を同じ向きにずらし、ダーティ
   フラグも一緒にずらす。これでスクロールは「ui.scroll 1 発 + 空行 1 行」
   だけになり、全画面再描画が消える (これが重さの主因だった)。 */
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
    dirty[scrollBot] = false; /* 空行: バッファも ui.scroll が BG で埋める */
    dirtySeq[scrollBot] = ++seq;
    ui.scroll(scrollTop, scrollBot, 1, BG);
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
    dirty[scrollTop] = false;
    dirtySeq[scrollTop] = ++seq;
    ui.scroll(scrollTop, scrollBot, -1, BG);
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
var csiParams = "";    /* "1;2;3" を貯める */
var csiPriv = false;   /* "?" 付き (DEC private) */

function params() {
    /* csiParams を整数配列に */
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
    } else { /* 0: カーソル→行末 */
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
    } else { /* 0: カーソル→画面末 */
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
            /* 256/truecolor: "5;n" か "2;r;g;b" を読み飛ばす */
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
    /* h/l (モード設定) や ? 付きは現状無視 (?25 カーソル表示等) */
}

/* 1 チャンクを処理 */
function feed(s) {
    for (var i2 = 0; i2 < s.length; i2++) {
        var ch = s[i2];
        var code = s.charCodeAt(i2);

        if (state === ST_OSC) {
            /* OSC: BEL か ST(ESC \) まで読み飛ばす */
            if (code === 7) state = ST_GROUND;
            else if (code === 27) state = ST_ESC; /* ESC \ の可能性 */
            continue;
        }
        if (state === ST_CHARSET) {
            state = ST_GROUND; /* G0/G1 指定の 1 バイトを捨てる */
            continue;
        }
        if (state === ST_ESC) {
            if (ch === "[") { state = ST_CSI; csiParams = ""; csiPriv = false; }
            else if (ch === "]") { state = ST_OSC; }
            else if (ch === "(" || ch === ")") { state = ST_CHARSET; }
            else if (ch === "M") { /* reverse index */
                if (cy === scrollTop) scrollDown(); else if (cy > 0) cy--;
                state = ST_GROUND;
            }
            else if (ch === "D") { lineFeed(); state = ST_GROUND; }
            else if (ch === "E") { cx = 0; lineFeed(); state = ST_GROUND; }
            else if (ch === "7") { savedCx = cx; savedCy = cy; state = ST_GROUND; }
            else if (ch === "8") { cx = savedCx; cy = savedCy; state = ST_GROUND; }
            else if (ch === "c") { /* full reset */
                resetTerm(); state = ST_GROUND;
            }
            else if (ch === "\\") { state = ST_GROUND; } /* ST 終端 */
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
            /* 中間バイト (0x20-0x2F) は無視 */
            continue;
        }

        /* ST_GROUND: 制御文字か印字 */
        if (code === 27) { state = ST_ESC; }
        else if (code === 13) { cx = 0; }                 /* CR */
        else if (code === 10) { lineFeed(); }             /* LF */
        else if (code === 8) { if (cx > 0) cx--; }        /* BS */
        else if (code === 9) {                            /* TAB */
            cx = clamp((cx + 8) & ~7, 0, COLS - 1);
        }
        else if (code === 7) { /* BEL: 無視 */ }
        else if (code < 32) { /* その他 C0: 無視 */ }
        else {
            /* 印字文字。autowrap */
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
/* 1 行を描画し、発行した ui コマンド数を返す。等幅セルは C 側が
   グリフを直接ブリットする (lv_draw_label を通さない)。同じ (fg,bg) の
   連続を 1 回の ui.cells にまとめるので、白文字の行なら 1 行 = 1 コマンド。 */
function drawRow(r) {
    var row = rows[r];
    var cmds = 0;
    var c = 0;
    while (c < COLS_VIS) {
        var f = row.fg[c], b = row.bg[c];
        var c2 = c + 1;
        while (c2 < COLS_VIS && row.fg[c2] === f && row.bg[c2] === b)
            c2++;
        ui.cells(c, r, row.ch.slice(c, c2), colorOf(f, FG), colorOf(b, BG));
        cmds++;
        c = c2;
    }
    return cmds;
}

/* バッファスクロールでスクロールが O(1) になったので、ダーティ行は
   常に少数。それでも一度に出し過ぎてキュー(128)を溢れさせないよう
   コマンド予算で律速しつつ、最新ダーティ行 (dirtySeq 最大) から描く。 */
var FLUSH_BUDGET = 90;
function flush() {
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
    if (cy < ROWS_VIS && cx < COLS_VIS)
        ui.rect(cx * CW, cy * LH + LH - 3, CW, 2, CURSOR);
}

/* ---- セルフテスト (PC) ---- */
function dumpGrid() {
    for (var r = 0; r < ROWS; r++) {
        var line = rows[r].ch;
        /* 末尾空白を削る */
        var e = line.length;
        while (e > 0 && line[e - 1] === " ") e--;
        print(("0" + r).slice(-2) + "|" + line.slice(0, e));
    }
    print("cursor=(" + cx + "," + cy + ") cols_vis=" + COLS_VIS +
          " rows_vis=" + ROWS_VIS + " cw=" + CW + " lh=" + LH);
}

/* グリッドを "rN|text" 行の配列に (末尾空白を除く非空行のみ) */
function gridLines() {
    var out = [];
    for (var r = 0; r < ROWS; r++) {
        var line = rows[r].ch;
        var e = line.length;
        while (e > 0 && line[e - 1] === " ") e--;
        if (e > 0)
            out.push(("0" + r).slice(-2) + "|" + line.slice(0, e));
    }
    return out;
}

if (SELFTEST) {
    feed(DEMO_SCRIPT);
    var lines = gridLines();
    for (var li = 0; li < lines.length; li++)
        print(lines[li]);
    print("END cursor=" + cx + "," + cy + " cols_vis=" + COLS_VIS +
          " rows_vis=" + ROWS_VIS + " cw=" + CW + " lh=" + LH);
} else if (REPORT) {
    /* 実機デモ (SSH/MQTT 不要): 台本を描画し、その後ずっと色付きの行を
       流してスクロール・カーソル・SGR が画面で動くのを見せる。 */
    ui.clear(BG);
    feed(DEMO_SCRIPT);
    feed("\r\n");
    setInterval(flush, 25);
    var tick = 0;
    setInterval(function () {
        tick++;
        var col = 31 + (tick % 7);       /* 色を巡回 */
        feed("\x1b[" + col + "mline " + tick +
             "\x1b[0m  scroll/cursor/SGR ok\r\n");
    }, 500);
} else {
    ui.clear(BG);
    ui.keyboard(1);

    ssh.onData(function (chunk) {
        feed(chunk);
    });
    ssh.onClose(function (reason) {
        feed("\r\n*** SSH closed: " + reason + " ***\r\n");
    });
    ui.onKey(function (k) {
        if (k === "\n")
            ssh.write("\r");
        else if (k === "\b")
            ssh.write("\x7f");
        else
            ssh.write(k);
    });

    setInterval(flush, 25); /* ~40fps でダーティ行を消化 */
    ssh.connect(HOST, PORT, USER, PASS, COLS, ROWS);
}
