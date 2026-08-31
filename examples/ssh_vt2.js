// @app ssh_vt2
// @title SSH端末 (native)
// @icon 
// @desc VT をプラットフォーム (term.*) が持つ SSH 端末。タブ 3 本・選択コピー・セッション永続。
// @perm ssh,clipboard,store,vault
/* Tab5 SSH 端末 — ネイティブ term.* 版 (docs/term-design.md §11.4)。
 * ssh_vt.js は**現役のまま**残る落とし所で、これはその置き換え候補
 * (アプリ名が別なので両方入れておける。詳細は examples/README.md)。
 *
 * JS に残るのは chrome だけ: タブ (= term.show の付け替え、§8)、接続 UX、
 * 長押し選択 → clipboard (画面は term.snapshot で読む)、ui.onKey →
 * ssh.write、回転時の term.resize + ssh.resize (pty 通知は呼び出し側の
 * 責務 — §8)。VT パーサ・grid・SGR・カーソル・スクロールバックは C で、
 * 受信バイトは term.pipe(tid, sshId) で JS を一度も通らない (§5, §9.1) —
 * だから ssh.onData は張らない (piped 中は発火しない)。IME も持ち物では
 * ない (§10): ui.ime(1) の opt-in だけで、確定文字列は ui.onKey に普通の
 * 文字列で届き、変換フロートのアンカーは C の term が caret シンクへ直接
 * push する (§10.2 — ui.caret を呼ぶ場所はこのアプリに無い)。
 *
 * mquickjs の罠: ブロックの中では必ず `var f = function(){}`。block-level
 * な function 宣言は timer/callback から見えず、発火時に ReferenceError。
 *
 * SELFTEST=true … PC (run_pc) で term.* 越しに自己診断を print する。 */
"use strict";
sys.setAppName("ssh_vt2");

var HOST = "192.168.1.10";
var PORT = 22;
var USER = "user";

var SELFTEST = false; /* PC: パーサ/選択/タブの自己診断を print して終わる */

/* ---- 画面メトリクス ----------------------------------------------------
   端末の桁数/行数はハードコードせず画面とセル寸法から導く。最上段
   TAB_ROWS 行はタブバー、その下が term.show の領域。 */
var TAB_ROWS = 1;
/* term_core.h の確保上限 (max_cols/max_rows/max_cells の既定値)。超えた
   resize は UI タスク側で静かに拒まれるだけで JS にはエラーが返らない
   ので、要求する前にこちらで丸める。 */
var MAX_COLS = 142, MAX_ROWS = 53, MAX_CELLS = 4260;
var CW = 9, LH = 24;
var W = 720, H = 1192, KB_H = 480;
var COLS = 80, ROWS = 28, GRID_ROWS = 29;
var SP = "";

/* 純関数にしてある: セルフテストが実機の寸法を作らずに検算できる。 */
function gridFor(w, h, kb, cw, lh) {
    var cols = (w / cw) | 0;
    var rows = (((h - kb) / lh) | 0) - TAB_ROWS;
    if (cols < 1) cols = 1;
    if (cols > MAX_COLS) cols = MAX_COLS;
    if (rows < 1) rows = 1;
    if (rows > MAX_ROWS) rows = MAX_ROWS;
    while (cols * rows > MAX_CELLS && rows > 1)
        rows--;
    return [cols, rows];
}

/* 負数 = 純クエリ (表示を変えない)。0 は「まだ答えられない」であって
   「キーボードが無い」ではないので直前の値を保つ — そのまま信じると
   キーの裏まで行を敷く。 */
function kbReserved() {
    var v = ui.keyboard(-2);
    return v > 0 ? v : KB_H;
}

function measure() {
    var sz = ui.size();
    var cell = ui.cellSize();
    W = sz[0] || W;
    H = sz[1] || H;
    CW = cell[0] || CW;
    LH = cell[1] || LH;
    KB_H = kbReserved();
    var g = gridFor(W, H, KB_H, CW, LH);
    COLS = g[0];
    ROWS = g[1];
    GRID_ROWS = ROWS + TAB_ROWS;
    SP = " ".repeat(COLS);
}

/* ---- 配色 (タブバーと選択だけ。端末の色は C が持つ) ---- */
var BG = 0x0B0E11;
var TAB_BG = 0x1A222C;
var TAB_FG = 0x8B98A5;
var TAB_ACT_BG = 0x2E6BD6;
var TAB_ACT_FG = 0xFFFFFF;
var SEL_FG = 0x000000, SEL_BG = 0x9CC4E4;
/* 記録中のタブ。青系のタブ色から一番遠い赤で、非アクティブでも赤のまま
   にする — 「録っているのはどのタブか」は選択中かどうかと無関係。 */
var REC_BG = 0xB3261E, REC_ACT_BG = 0xE04A3F, REC_FG = 0xFFFFFF;
var REC_DOT = "●";   /* ● */

/* ---- snapshot の行を「列 → 文字」へ展開する ---------------------------
   term.snapshot は CONT セル (幅 2 文字の右半分) を落とした UTF-8 を
   返す。列座標で選択するにはこちらで幅を足し直す必要があり、ui.cells
   へ渡すにも「1 列 1 コードポイント + 右半分の filler」が要る。 */
function expandRow(line, cols) {
    var ch = new Array(cols), cont = new Array(cols);
    var c, i = 0;
    for (c = 0; c < cols; c++) {
        ch[c] = " ";
        cont[c] = false;
    }
    c = 0;
    if (!line)
        return { ch: ch, cont: cont };
    while (i < line.length && c < cols) {
        var code = line.charCodeAt(i), cp = code, n = 1;
        if (code >= 0xD800 && code <= 0xDBFF && i + 1 < line.length) {
            var lo = line.charCodeAt(i + 1);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((code - 0xD800) << 10) + (lo - 0xDC00);
                n = 2;
            }
        }
        var w = ui.cellWidth(cp);
        i += n;
        if (w === 0)
            continue; /* 結合文字: 列を進めない (C の grid も同じ扱い) */
        ch[c] = line.slice(i - n, i);
        c++;
        if (w === 2 && c < cols) {
            ch[c] = " ";
            cont[c] = true;
            c++;
        }
    }
    return { ch: ch, cont: cont };
}

/* 列 [c0, c1] を文字列に。copy=true は filler を落とす (コピー用)、
   false は空白として残す (ui.cells の 1 列 1 コードポイント契約)。 */
function rowText(row, c0, c1, copy) {
    var out = "";
    for (var c = c0; c <= c1; c++) {
        if (row.cont[c]) {
            if (!copy)
                out += " ";
        } else {
            out += row.ch[c];
        }
    }
    return out;
}

/* ---- タブバーの見た目 (純関数、セルフテストが検算する) ----------------
   戻り: {runs:[{c,text,fg,bg}], hits:[{x0,x1,idx}]}  idx -1 = [+]。

   rec[i] が真のタブは「記録中」。§4.4 の記録モードはユーザーが「録って
   いるかどうか」を画面だけで判断できることが前提の契約なので、見落と
   しようのない出し方にする:
     ・そのタブのラベル先頭に ● を付ける
     ・そのタブの背景を赤にする (非アクティブでも赤)
     ・アクティブタブが記録中なら右端に ●REC を出す (修飾キー表示の左)
   3 つ重ねているのは冗長ではなく、ラベルが長くて切れた場合・タブが
   はみ出して並ばない場合それぞれに一つずつ残るため。 */
function tabLayout(labels, actIdx, cols, cw, canAdd, mods, rec) {
    var runs = [], hits = [], c = 0, i;
    for (i = 0; i < labels.length; i++) {
        var on = !!(rec && rec[i]);
        var label = " " + (on ? REC_DOT : "") + (i + 1) + ":" + labels[i] + " ";
        if (label.length > 22)
            label = label.slice(0, 21) + "… ";
        /* -4 は [+] (3 セル) の取り置き。モード表示は制御バーの「あ」キー
           の面へ移ったので、その 4 桁はタブ名に戻っている。 */
        if (c + label.length >= cols - 4)
            break;
        var act = (i === actIdx);
        runs.push({ c: c, text: label,
                    fg: on ? REC_FG : (act ? TAB_ACT_FG : TAB_FG),
                    bg: on ? (act ? REC_ACT_BG : REC_BG)
                           : (act ? TAB_ACT_BG : TAB_BG) });
        hits.push({ x0: c * cw, x1: (c + label.length) * cw, idx: i });
        c += label.length;
    }
    if (canAdd) {
        runs.push({ c: c, text: " + ", fg: TAB_ACT_FG, bg: 0x256B45 });
        hits.push({ x0: c * cw, x1: (c + 3) * cw, idx: -1 });
    }
    var modsAt = cols - (mods ? mods.length : 0);
    if (mods && cols > mods.length)
        runs.push({ c: modsAt, text: mods, fg: 0x000000, bg: 0xFFD479 });
    if (rec && actIdx >= 0 && rec[actIdx]) {
        var badge = " " + REC_DOT + "REC ";
        if (modsAt - badge.length > c)
            runs.push({ c: modsAt - badge.length, text: badge,
                        fg: REC_FG, bg: REC_BG });
    }
    return { runs: runs, hits: hits };
}

measure();

if (SELFTEST) {
    /* ---- PC 自己診断 (SSH 不要) ---------------------------------------
       run_pc のループはキューされたイベントより先にタイマーを回すので、
       feed の結果を「次の 1 ティック」で決め打ちしてはいけない。
       snapshot をポーリングして待つ。 */
    var fails = 0;
    var ok = function (name, cond) {
        print((cond ? "ok   " : "FAIL ") + name);
        if (!cond)
            fails++;
    };
    var SC = 20, SR = 5;
    var tid = term.create({ name: "self", mode: "vt", persist: false,
                            cols: SC, rows: SR });
    ok("term.create vt", tid > 0);
    var snapRow = function (r) {
        var s = term.snapshot(tid);
        if (!s)
            return null;
        var lines = s.split("\n");
        return r < lines.length ? lines[r] : "";
    };
    var poll = function (want, tries, next) {
        var r0 = snapRow(0);
        if ((r0 !== null && r0.indexOf(want) === 0) || tries <= 0) {
            next(r0);
            return;
        }
        setTimeout(function () { poll(want, tries - 1, next); }, 20);
    };
    var pollBlank = function (tries, next) {
        var s = term.snapshot(tid);
        if ((s !== null && !/\S/.test(s)) || tries <= 0) {
            next(s);
            return;
        }
        setTimeout(function () { pollBlank(tries - 1, next); }, 20);
    };
    term.feed(tid, "\x1b[2J\x1b[Habc\r\n日本語x\r\n\x1b[1;32mgrn");
    poll("abc", 60, function (r0) {
        ok("feed -> snapshot row0", r0 === "abc");
        var r1 = snapRow(1);
        ok("row1 wide text", r1 === "日本語x");
        var ex = expandRow(r1, SC);
        ok("wide cell occupies 2 cols", ex.cont[1] === true &&
           ex.cont[3] === true && ex.cont[5] === true);
        ok("col0 is the wide char", ex.ch[0] === "日");
        ok("col6 after 3 wide chars", ex.ch[6] === "x");
        ok("copy text drops fillers",
           rowText(ex, 0, 3, true) === "日本");
        ok("draw text keeps fillers",
           rowText(ex, 0, 3, false) === "日 本 ");
        ok("sgr row is plain text in snapshot", snapRow(2) === "grn");
        /* 選択 2 行ぶんの抽出 (アプリの selText と同じ手順) */
        var lines = [expandRow(snapRow(0), SC), expandRow(snapRow(1), SC)];
        var out = [rowText(lines[0], 1, SC - 1, true).trimEnd(),
                   rowText(lines[1], 0, 3, true).trimEnd()];
        ok("selection across rows",
           out.join("\n") === "bc\n日本");
        /* RIS: 切断済みタブを作り直す経路 */
        term.feed(tid, "\x1bc");
        pollBlank(60, function (s) {
            ok("RIS clears the screen", !s || !/\S/.test(s));
            /* pipe: 2 回目は BUSY + 旧 producer に detach 要求 (§5) */
            ok("pipe stub", term.pipe(tid, 1) === term.OK);
            ok("feed is BUSY while piped", term.feed(tid, "x") === term.BUSY);
            ok("re-pipe is BUSY", term.pipe(tid, 2) === term.BUSY);
            setTimeout(function () {
                ok("re-pipe after the ack", term.pipe(tid, 2) === term.OK);
                ok("unpipe", term.unpipe(tid) === term.OK);
                /* タブバー */
                var lay = tabLayout(["a@h", "b@h", "c@h"], 1, 80, 9, false,
                                    " CTRL ");
                ok("3 tabs + mods", lay.runs.length === 4);
                ok("active tab colour", lay.runs[1].bg === TAB_ACT_BG);
                ok("mods at the right edge",
                   lay.runs[3].c === 80 - " CTRL ".length);
                ok("hit boxes ordered",
                   lay.hits.length === 3 && lay.hits[0].x1 === lay.hits[1].x0);
                var lay2 = tabLayout([], -1, 80, 9, true, "");
                ok("[+] only", lay2.hits.length === 1 && lay2.hits[0].idx === -1);
                /* 記録中の見せ方: ● 付きラベル + 赤背景 + 右端 ●REC。
                   非アクティブでも赤のままであること。 */
                var lr = tabLayout(["a@h", "b@h"], 0, 80, 9, false, "",
                                   [true, false]);
                ok("recording tab is red", lr.runs[0].bg === REC_ACT_BG);
                ok("recording tab has the dot",
                   lr.runs[0].text.indexOf(REC_DOT) >= 0);
                ok("the other tab is not red", lr.runs[1].bg === TAB_BG);
                ok("REC badge present",
                   lr.runs[lr.runs.length - 1].text.indexOf("REC") >= 0);
                var lr2 = tabLayout(["a@h", "b@h"], 0, 80, 9, false, "",
                                    [false, true]);
                ok("an inactive recording tab stays red",
                   lr2.runs[1].bg === REC_BG);
                ok("no badge when the active tab is not recording",
                   lr2.runs[lr2.runs.length - 1].text.indexOf("REC") < 0);
                /* term.record の契約: 既定は切、1 引数は問い合わせ、
                   記録していないタブへの recordScreen は拒否。 */
                ok("recording is OFF by default", term.record(tid) === 0);
                ok("recordScreen refused while off",
                   term.recordScreen(tid) === term.INVAL);
                ok("record on", term.record(tid, true) === term.OK);
                ok("query says on", term.record(tid) === 1);
                ok("recordScreen accepted", term.recordScreen(tid) === term.OK);
                ok("record off", term.record(tid, false) === term.OK);
                ok("query says off again", term.record(tid) === 0);
                ok("a bad id does not throw", term.record(999999) < 0);
                /* 記録はクラス B の例外 = VT 限定。LOG term は既に
                   クラス A で黒箱に入っているので二重記録になる。 */
                var lg = term.create({ name: "selflog", mode: "log",
                                       cols: SC, rows: SR });
                ok("record on a LOG term is MODE",
                   term.record(lg, true) === term.MODE);
                ok("close log", term.close(lg) === term.OK);
                /* メトリクス */
                var p = gridFor(720, 1192, 480, 9, 24);
                var l = gridFor(1192, 720, 80, 9, 24);
                var big = gridFor(4000, 4000, 0, 9, 24);
                ok("portrait grid", p[0] === 80 && p[1] === 28);
                ok("landscape grid", l[0] === 132 && l[1] === 25);
                ok("clamped to the core's maxima",
                   big[0] <= MAX_COLS && big[1] <= MAX_ROWS &&
                   big[0] * big[1] <= MAX_CELLS);
                ok("close", term.close(tid) === term.OK);
                print(fails ? ("SSHVT2 SELFTEST: " + fails + " FAILED")
                            : "SSHVT2 SELFTEST: ALL PASS");
            }, 60);
        });
    });
} else {
    /* ====================== SSH クライアント ============================ */
    var inForm = false;
    var HOSTS_KEY = "ssh_hosts";   /* store は全アプリ共有 = ssh_vt と同じ一覧 */
    var TABS_KEY = "svt2_tabs";    /* 再起動を跨いだタブの復元記録 */
    var MAX_SESS = 3;              /* == SSHC_MAX_SESSIONS */
    /* term の name は「同一 owner 内での再アタッチ鍵」(§3.1)。タブ位置では
       なく固定の 3 つを使い回す — アプリを停止しても persist term は
       DETACHED で残り、同じ名前の create が拾い直す (tmux モデル)。 */
    var TAB_NAMES = ["t0", "t1", "t2"];
    /* term.pipe の再試行予算。ssh.connect は id を即返し、認証と shell
       要求はセッションタスクで進むので最初の pipe は必ず早すぎる
       (term_pipe.h: TERM_ERR_NOT_READY)。WiFi + tailnet 越しのログインは
       秒単位なので 20 × 300ms = 6 秒みておく。 */
    var PIPE_TRIES = 20, PIPE_WAIT_MS = 300;

    /* sessions[i] = {name, tid, sshId, live, label} — 並びがタブ順 */
    var sessions = [];
    var actIdx = -1;
    var tabHit = [];
    var pendCtrl = false, pendAlt = false;

    var credName = function (e) {
        return "ssh-pass:" + e.user + "@" + e.host + ":" + e.port;
    };
    var hostKeyName = function (e) {
        return "ssh-hostkey:" + e.host + ":" + e.port;
    };
    var hostLabel = function (e) {
        return e.user + "@" + e.host + ":" + e.port;
    };
    var unwind = function () {
        while (ui.back()) {}
    };
    var loadHosts = function () {
        var s = store.get(HOSTS_KEY);
        if (!s)
            return [];
        try {
            var a = JSON.parse(s);
            if (!a || a.length === undefined)
                return [];
            return a;
        } catch (e) {
            return [];
        }
    };
    var saveHosts = function (hosts) {
        store.set(HOSTS_KEY, JSON.stringify(hosts));
    };
    var saveTabs = function () {
        var rec = [];
        for (var i = 0; i < sessions.length; i++)
            rec.push({ n: sessions[i].name, label: sessions[i].label });
        store.set(TABS_KEY, JSON.stringify(rec));
    };

    /* ---- term の表示領域 ------------------------------------------------
       タブバーの下、キーボードの上。C 側は view.x/y をセル座標に割るので
       セル境界で渡す。 */
    var termRect = function () {
        return { x: 0, y: TAB_ROWS * LH, w: COLS * CW, h: ROWS * LH };
    };
    var hideAll = function () {
        for (var i = 0; i < sessions.length; i++)
            term.show(sessions[i].tid, null);
    };
    /* 全面再描画の要求。C の damage 集合は「キャンバスをアプリが消した」
       ことを知らないので、hide → show で可視化遷移を作って全面再描画を
       もらう (§8: タブ切替は show の付け替え)。 */
    var repaintActive = function () {
        if (actIdx < 0 || actIdx >= sessions.length)
            return;
        term.show(sessions[actIdx].tid, null);
        term.show(sessions[actIdx].tid, termRect());
    };

    var drawTabs = function () {
        var labels = [], rec = [], i;
        for (i = 0; i < sessions.length; i++) {
            labels.push((sessions[i].live ? "" : "×") + sessions[i].label);
            /* 記録状態は C に毎回聞く。キャッシュしないのは意図的で、
               プラットフォームは再アタッチ・デタッチ・pipe・セッション
               終了で勝手に (正しく) 記録を落とすから (term_registry.h
               R1)。持っている bool を信じると「録っていないのに ● が
               点いている」= 契約の逆を表示することになる。 */
            rec.push(term.record(sessions[i].tid) === 1);
        }
        var mods = (pendCtrl ? " CTRL " : "") + (pendAlt ? " ALT " : "");
        var lay = tabLayout(labels, actIdx, COLS, CW,
                            sessions.length < MAX_SESS, mods, rec);
        ui.cells(0, 0, SP, TAB_FG, TAB_BG); /* 行クリア */
        for (i = 0; i < lay.runs.length; i++)
            ui.cells(lay.runs[i].c, 0, lay.runs[i].text,
                     lay.runs[i].fg, lay.runs[i].bg);
        tabHit = lay.hits;
    };

    var sel = null;       /* {a:{c,r}, b:{c,r}} 端末セル座標 (a=起点) */
    var selRows = null;   /* snapshot のキャッシュ (展開済み) */

    var switchTo = function (i) {
        if (i < 0 || i >= sessions.length)
            return;
        if (actIdx >= 0 && actIdx < sessions.length &&
            actIdx !== i)
            term.show(sessions[actIdx].tid, null);
        sel = null;
        selRows = null;
        actIdx = i;
        term.show(sessions[i].tid, null);   /* 可視化遷移 = 全面再描画 */
        term.show(sessions[i].tid, termRect());
        drawTabs();
    };

    var closeTab = function (i) {
        if (i < 0 || i >= sessions.length)
            return;
        term.close(sessions[i].tid);
        if (sessions[i].sshId)
            ssh.close(sessions[i].sshId);
        sessions.splice(i, 1);
        if (actIdx >= sessions.length)
            actIdx = sessions.length - 1;
        else if (i < actIdx)
            actIdx--;
        sel = null;
        saveTabs();
    };

    /* ---- レイアウト変更 (回転・ドック抜き差し・キーボードモード) ------- */
    var relayout = function () {
        measure();
        for (var i = 0; i < sessions.length; i++) {
            term.resize(sessions[i].tid, COLS, ROWS);
            /* pty への通知は呼び出し側の責務 (§8)。切断済み id に投げても
               C が捨てる。 */
            if (sessions[i].live)
                ssh.resize(sessions[i].sshId, COLS, ROWS);
        }
        /* 選択はセル座標で持っている。ROWS が縮むと範囲外を触るので捨てる
           (ドックを挿した指が画面に乗ったままの回転で実際に起きる)。 */
        sel = null;
        selRows = null;
        if (inForm)
            return; /* フォーム表示中は寸法だけ直す */
        ui.clear(BG);
        if (actIdx >= 0 && actIdx < sessions.length) {
            repaintActive();
            drawTabs();
        }
    };

    /* ---- キー (T3a のトークン表 + one-shot 修飾) ---------------------- */
    var TOKSEQ = {
        esc: "\x1b", tab: "\t",
        up: "\x1b[A", down: "\x1b[B", right: "\x1b[C", left: "\x1b[D",
        home: "\x1b[H", end: "\x1b[F",
        pgup: "\x1b[5~", pgdn: "\x1b[6~", del: "\x1b[3~", ins: "\x1b[2~",
        f1: "\x1bOP", f2: "\x1bOQ", f3: "\x1bOR", f4: "\x1bOS",
        f5: "\x1b[15~", f6: "\x1b[17~", f7: "\x1b[18~", f8: "\x1b[19~",
        f9: "\x1b[20~", f10: "\x1b[21~", f11: "\x1b[23~", f12: "\x1b[24~"
    };

    var confirmPaste = null; /* 下で代入 (フォーム群はまとめて後ろ) */

    /* ここへ来る打鍵は既に IME を通り抜けたもの (素通しのキーか、確定した
       日本語の文字列)。「あ」の "\x00ime" も C が食う。
       ⚠️ 確定した日本語を bracketed paste で囲んではいけない。 */
    var onKey = function (k) {
        if (k.charCodeAt(0) === 0 && k.slice(1) === "rotate") {
            relayout();
            return;
        }
        if (actIdx < 0)
            return;
        var s = sessions[actIdx];
        /* 打鍵したら生きている画面へ戻す。履歴を見ている最中に入力だけ
           通ると、自分の打った文字がどこにも出ない。
           term.scroll(id, 0) は「動かさずに読む」。 */
        var back = term.scroll(s.tid, 0);
        if (back > 0)
            term.scroll(s.tid, -back);
        if (k.charCodeAt(0) === 0) {
            var name = k.slice(1);
            if (name === "ctrl") { pendCtrl = !pendCtrl; drawTabs(); return; }
            if (name === "alt") { pendAlt = !pendAlt; drawTabs(); return; }
            if (name === "copy" || name === "paste") {
                if (pendCtrl || pendAlt) {
                    pendCtrl = pendAlt = false;
                    drawTabs();
                }
                if (name === "copy") {
                    sys.notify("画面を長押し→なぞってコピー");
                    return;
                }
                if (!s.live) {
                    sys.notify("このタブは切断済みです");
                    return;
                }
                var clip = clipboard.get();
                if (clip && clip.data) {
                    /* DECSET 2004 の状態は C の term_core が持っていて JS へ
                       出ていない (term.modes 相当が無い) ので、ここでは
                       囲まない。制御文字入りの貼り付けは確認画面で止める。 */
                    if (/[\r\n\x00-\x08\x0b\x0c\x0e-\x1f\x7f]/.test(clip.data))
                        confirmPaste(s.sshId, clip.data);
                    else
                        ssh.write(s.sshId, clip.data);
                }
                return;
            }
            k = TOKSEQ[name] || "";
            if (!k)
                return;
        } else if (k === "\n") {
            k = "\r";
        } else if (k === "\b") {
            k = "\x7f";
        }
        if (!s.live) {
            sys.notify("このタブは切断済みです");
            return;
        }
        if (pendCtrl) {
            pendCtrl = false;
            if (k === " ")
                k = "\x00";
            else if (k.length === 1 && k.charCodeAt(0) >= 64)
                k = String.fromCharCode(k.charCodeAt(0) & 0x1F);
            drawTabs();
        }
        if (pendAlt) {
            pendAlt = false;
            k = "\x1b" + k;
            drawTabs();
        }
        ssh.write(s.sshId, k);
    };
    ui.onKey(onKey);
    /* 日本語入力の opt-in はこれだけ (§10)。 */
    ui.ime(1);

    /* ---- 長押し選択 → clipboard ---------------------------------------
       画面は term.snapshot から読む (§11.4: 選択は grid 読み出し API で
       足りる)。ハイライトは ui.cells の重ね描きで、C の再描画に消されても
       下のティックが引き直す。 */
    var LP_MS = 500;
    var press = null;
    var selAge = 0;

    var cellAt = function (x, y) {
        var c = (x / CW) | 0;
        var r = ((y / LH) | 0) - TAB_ROWS;
        if (c < 0) c = 0;
        if (c >= COLS) c = COLS - 1;
        if (r < 0) r = 0;
        if (r >= ROWS) r = ROWS - 1;
        return { c: c, r: r };
    };
    var selRange = function () {
        var a = sel.a, b = sel.b;
        if (a.r < b.r || (a.r === b.r && a.c <= b.c))
            return [a, b];
        return [b, a];
    };
    /* 画面を 1 回だけ読んで列展開しておく。指を動かすたびに読み直すと
       UI タスクへの往復が増えるだけなので、変化時と数ティック毎に更新。 */
    var grabRows = function () {
        selRows = null;
        if (actIdx < 0)
            return;
        var s = term.snapshot(sessions[actIdx].tid);
        if (!s)
            return;
        var lines = s.split("\n"), out = [];
        for (var r = 0; r < ROWS; r++)
            out.push(expandRow(r < lines.length ? lines[r] : "", COLS));
        selRows = out;
        selAge = 0;
    };
    var drawSel = function () {
        if (!sel || !selRows || actIdx < 0)
            return;
        var fr = selRange(), f = fr[0], to = fr[1];
        for (var r = f.r; r <= to.r && r < selRows.length; r++) {
            var c0 = (r === f.r) ? f.c : 0;
            var c1 = (r === to.r) ? to.c : COLS - 1;
            /* 幅 2 文字の左半分で離すと右半分が反転から漏れる。CONT セルまで
               伸ばして 1 文字まるごと反転させる (コピー範囲は変わらない)。 */
            if (c1 + 1 < COLS && selRows[r].cont[c1 + 1])
                c1++;
            ui.cells(c0, r + TAB_ROWS, rowText(selRows[r], c0, c1, false),
                     SEL_FG, SEL_BG);
        }
    };
    var selText = function () {
        if (!sel || !selRows)
            return "";
        var fr = selRange(), f = fr[0], to = fr[1], out = [];
        for (var r = f.r; r <= to.r && r < selRows.length; r++) {
            var c0 = (r === f.r) ? f.c : 0;
            var c1 = (r === to.r) ? to.c : COLS - 1;
            out.push(rowText(selRows[r], c0, c1, true).trimEnd());
        }
        return out.join("\n");
    };
    var endPress = function () {
        if (press && press.timer !== null)
            clearTimeout(press.timer);
        press = null;
    };
    var dropSel = function () {
        sel = null;
        selRows = null;
        repaintActive(); /* ハイライトを剥がすのは C の全面再描画 */
    };

    ui.onTouch(function (x, y, kind) {
        if (inForm)
            return;
        if (kind === 0) {
            if (sel)
                dropSel();
            if (y < TAB_ROWS * LH + 8) {
                for (var i = 0; i < tabHit.length; i++) {
                    if (x >= tabHit[i].x0 && x < tabHit[i].x1) {
                        if (tabHit[i].idx < 0)
                            hostsPage(null);   /* [+] */
                        else if (tabHit[i].idx === actIdx)
                            tabMenu(actIdx);   /* 現在のタブ = タブメニュー */
                        else
                            switchTo(tabHit[i].idx);
                        return;
                    }
                }
                return;
            }
            endPress();
            if (y >= GRID_ROWS * LH)
                return; /* 予約領域 (キーボード / パネル) のタップは拾わない */
            press = { x: x, y: y, moved: false, timer: null };
            if (actIdx >= 0) {
                press.timer = setTimeout(function () {
                    if (press === null || press.moved)
                        return;
                    press.timer = null;
                    var c0 = cellAt(press.x, press.y);
                    sel = { a: c0, b: c0 };
                    grabRows();
                    drawSel(); /* 点灯 = 選択モードのフィードバック */
                }, LP_MS);
            }
            return;
        }
        if (kind === 1) {
            if (sel) {
                var cur = cellAt(x, y);
                if (cur.c !== sel.b.c || cur.r !== sel.b.r) {
                    sel.b = cur;
                    /* 縮んだぶんを剥がすのは全面再描画。引き直しは次の
                       ティック (repaint は次フレームなので順序が要る)。 */
                    repaintActive();
                }
            } else if (press) {
                var dx = x - press.x, dy = y - press.y;
                if (!press.moved && dx * dx + dy * dy > 144) {
                    /* 12px 動いた = 長押しではない。ここから先はスクロール。
                       選択は長押し、スクロールはドラッグ、と役割を分ける。 */
                    press.moved = true;
                    press.sy = y;
                    if (press.timer !== null) {
                        clearTimeout(press.timer);
                        press.timer = null;
                    }
                }
                if (press.moved && actIdx >= 0) {
                    /* 指を下へ = 紙を下へ引く = 古い行が出てくる。
                       段の高さで量子化し、残りは press.sy に持ち越す
                       (毎フレーム端数を捨てると指の速度で挙動が変わる)。 */
                    var lines = Math.floor((y - press.sy) / LH);
                    if (lines !== 0) {
                        press.sy += lines * LH;
                        term.scroll(sessions[actIdx].tid, lines);
                    }
                }
            }
            return;
        }
        /* kind 2: 離した */
        if (sel) {
            var text = selText();
            dropSel();
            if (text) {
                clipboard.set(text, "text/plain");
                sys.notify("コピー: " + text.length + " 文字");
            }
            endPress();
            return;
        }
        if (press && !press.moved)
            ui.keyboard(2); /* 短タップ = キーボード呼び戻し */
        endPress();
    });

    /* ---- ティック: レイアウト追従と選択ハイライトの自己修復 ----------
       端末の描画そのものは C の UI フレームフックがやるので、ここには
       flush も carets も無い。 */
    setInterval(function () {
        var s2 = ui.size();
        if (s2[0] !== W || s2[1] !== H || kbReserved() !== KB_H) {
            relayout();
            return;
        }
        if (sel) {
            if (++selAge >= 4)
                grabRows(); /* 受信で画面が変わっていたら追従 */
            drawSel();
        }
    }, 60);

    /* ---- セッション ---------------------------------------------------- */
    var idxOf = function (entry) {
        for (var i = 0; i < sessions.length; i++)
            if (sessions[i] === entry)
                return i;
        return -1;
    };

    /* term.pipe は producer が居れば BUSY を返し、そのとき旧 producer へ
       detach を要求する (§5 / PHASE4 Decision 2: ack join は呼び出し側の
       再試行)。ack は ssh セッションタスクから 1 recv timeout で届く。 */
    var pipeWithRetry = function (entry, tries) {
        if (!entry.sshId || idxOf(entry) < 0)
            return;
        var rc = term.pipe(entry.tid, entry.sshId);
        if (rc === term.OK) {
            print("ssh_vt2: piped tab " + entry.name + " after " +
                  (PIPE_TRIES - tries) + " tr" + (PIPE_TRIES - tries === 1 ? "y" : "ies"));
            return;
        }
        /* NOT_READY = the handshake is still running (ssh.connect hands back
           an id immediately, so the first attempt is always early); BUSY =
           the old producer has just been asked to detach. Both are "ask
           again", and the budget has to cover a real login over WiFi, not
           a few hundred ms. Anything else is final. */
        if ((rc === term.NOT_READY || rc === term.BUSY) && tries > 0) {
            setTimeout(function () { pipeWithRetry(entry, tries - 1); },
                       PIPE_WAIT_MS);
            return;
        }
        /* print() as well as notify(): the toast is gone in five seconds,
           the LP black box keeps the line across a reboot and a pull. */
        print("ssh_vt2: term.pipe failed rc=" + rc + " tab=" + entry.name +
              " sshId=" + entry.sshId + " triesLeft=" + tries);
        sys.notify("端末に繋げない (term.pipe " + rc + ")");
    };

    /* 空きタブ名、無ければ切断済みタブの使い回し (画面は RIS で消す)。 */
    var pickTab = function () {
        var used = {}, i;
        for (i = 0; i < sessions.length; i++)
            used[sessions[i].name] = 1;
        for (i = 0; i < TAB_NAMES.length; i++) {
            if (used[TAB_NAMES[i]])
                continue;
            var tid = term.create({ name: TAB_NAMES[i], mode: "vt",
                                    persist: true, cols: COLS, rows: ROWS });
            if (tid < 0)
                return null;
            return { name: TAB_NAMES[i], tid: tid, reuse: -1 };
        }
        for (i = 0; i < sessions.length; i++) {
            if (!sessions[i].live) {
                term.feed(sessions[i].tid, "\x1bc"); /* RIS */
                return { name: sessions[i].name, tid: sessions[i].tid,
                         reuse: i };
            }
        }
        return null;
    };

    var onSessionClosed = function (entry, reason) {
        var i = idxOf(entry);
        if (i < 0)
            return;
        entry.live = false;
        entry.sshId = 0;
        /* 中身が残っていない (認証で落ちた等) タブは畳む。残っていれば
           「切断済みだが画面は読める」タブとして持つ — persist term の
           値打ちはそこ。 */
        var snap = term.snapshot(entry.tid);
        if (!snap || !/\S/.test(snap) || reason.indexOf("hostkey") === 0)
            closeTab(i);
        else
            saveTabs();
        var live = 0;
        for (var k = 0; k < sessions.length; k++)
            if (sessions[k].live)
                live++;
        if (!live) {
            actIdx = sessions.length ? 0 : -1;
            if (actIdx >= 0)
                switchTo(0);
            ui.keyboard(0);
            hostsPage("切断: " + entry.label + " (" + reason + ")");
            return;
        }
        if (actIdx < 0 || actIdx >= sessions.length)
            switchTo(sessions.length - 1);
        else
            drawTabs();
    };

    var startSession = function (e) {
        var slot = pickTab();
        if (!slot) {
            hostsPage("タブがいっぱいです (" + MAX_SESS + " 本)");
            return;
        }
        var id;
        try {
            id = ssh.connect(e.host, e.port, e.user, credName(e),
                             hostKeyName(e), COLS, ROWS);
        } catch (err) {
            if (slot.reuse < 0)
                term.close(slot.tid);
            hostsPage("接続できない: " + err.message);
            return;
        }
        var entry;
        if (slot.reuse >= 0) {
            entry = sessions[slot.reuse];
            entry.sshId = id;
            entry.live = true;
            entry.label = e.user + "@" + e.host;
        } else {
            entry = { name: slot.name, tid: slot.tid, sshId: id, live: true,
                      label: e.user + "@" + e.host };
            sessions.push(entry);
        }
        term.resize(entry.tid, COLS, ROWS);
        pipeWithRetry(entry, PIPE_TRIES);
        saveTabs();

        var forgetTimer = null;
        if (e.transientCred) {
            forgetTimer = setInterval(function () {
                if (ssh.connected(id)) {
                    vault.del(credName(e));
                    clearInterval(forgetTimer);
                    forgetTimer = null;
                }
            }, 100);
        }
        ssh.onClose(id, function (reason) {
            if (forgetTimer !== null) {
                clearInterval(forgetTimer);
                forgetTimer = null;
            }
            if (reason.indexOf("hostkey-changed:") === 0 ||
                reason.indexOf("hostkey:") === 0) {
                onSessionClosed(entry, reason);
                trustHostPage(e, reason.slice(reason.indexOf(":") + 1),
                              reason.indexOf("hostkey-changed:") === 0);
                return;
            }
            if (e.transientCred)
                vault.del(credName(e));
            onSessionClosed(entry, reason);
        });

        inForm = false;
        unwind();
        ui.clear(BG);
        ui.keyboard(2);
        switchTo(idxOf(entry));
    };

    /* 再起動/停止を跨いだ復元 (§3.1 tmux モデル)。persist term は同じ
       owner+name の create で拾い直せる。中身が空なら「再アタッチでは
       なく新規」= 見せるものが無いので畳む。
       ⚠️ create が再アタッチだったかを JS は知れない (C の
       out_reattached が binding に出ていない) ので、記録は store 側で
       持ち、空判定で裏を取る。 */
    var restoreTabs = function () {
        var rec = [];
        try {
            rec = JSON.parse(store.get(TABS_KEY) || "[]");
        } catch (e) {
            rec = [];
        }
        if (!rec || rec.length === undefined)
            rec = [];
        for (var i = 0; i < rec.length && sessions.length < MAX_SESS; i++) {
            var n = rec[i].n;
            if (TAB_NAMES.indexOf(n) < 0)
                continue;
            var tid = term.create({ name: n, mode: "vt", persist: true,
                                    cols: COLS, rows: ROWS });
            if (tid < 0)
                continue;
            var snap = term.snapshot(tid);
            if (!snap || !/\S/.test(snap)) {
                term.close(tid);
                continue;
            }
            sessions.push({ name: n, tid: tid, sshId: 0, live: false,
                            label: rec[i].label || n });
        }
        saveTabs();
    };

    /* ---- フォーム群 (ホスト一覧・接続・ホスト鍵・危険な貼り付け) ------ */
    var returnTerminal = function () {
        inForm = false;
        unwind();
        ui.clear(BG);
        if (actIdx >= 0 && actIdx < sessions.length) {
            repaintActive();
            drawTabs();
            ui.keyboard(2);
        }
    };

    /* ---- タブメニュー (記録の入/切) ------------------------------------
       出し方はアクティブなタブをもう一度タップ。非アクティブのタップは
       今まで通り切り替えなので、既にある操作を潰していない。

       記録は §4.4 の 2026-07-30 例外。ここで説明しているのは「何が起きる
       か」ではなく「何を受け入れることになるか」で、それがこのページの
       主目的: セッションの表示内容が署名鍵の持ち主に平文で取り出せるよう
       になり、電源を切るまでリセットを跨いで残る。 */
    var tabMenu = function (i) {
        if (i < 0 || i >= sessions.length)
            return;
        var e = sessions[i];
        var on = term.record(e.tid) === 1;
        inForm = true;
        ui.keyboard(0);
        var s = ui.screen("タブ " + (i + 1) + ": " + e.label);
        s.label(on ? "● 記録中 — このセッションの表示内容を黒箱に残しています"
                   : "記録: 切 (既定)");
        if (on) {
            s.label("PC から: tools/bb_pull.py <host> <topic> live --session");
            s.button("今の画面を記録する", function () {
                var rc = term.recordScreen(e.tid);
                print("ssh_vt2: record screen tab=" + e.name + " rc=" + rc);
                sys.notify(rc === term.OK ? "画面を記録しました"
                                          : ("記録できない (" + rc + ")"));
                returnTerminal();
            });
            s.button("記録を停止する", function () {
                var rc = term.record(e.tid, false);
                /* print も残す: トーストは 5 秒で消えるが、記録の入/切
                   そのものが黒箱に残っていないと、後から transcript を
                   読む人が「どこからどこまで録られていたか」を判断でき
                   ない (Defect C の教訓の一般形)。 */
                print("ssh_vt2: recording OFF tab=" + e.name + " rc=" + rc);
                sys.notify("記録を停止しました");
                returnTerminal();
            });
        } else {
            s.label("入にすると、この画面に出る内容が LP 黒箱に残ります。");
            /* 直感に反する方が危ない側なので明示する: 開始時に可視画面を
               1 枚撮る (エラーを見てから入にして拾う用途がこれ)。つまり
               「秘密を出した後で入にした」は安全ではない。 */
            s.label("いま表示中の画面も、開始した時点で保存されます。");
            s.label("署名鍵の持ち主は MQTT 経由で平文で取り出せます。");
            s.label("電源を切るまでリセットを跨いで残ります。");
            s.label("秘密を表示するセッションでは入にしないでください。");
            s.label("次のセッションでは必ず切に戻ります (記憶しません)。");
            s.button("記録を開始する (このセッションのみ)", function () {
                var rc = term.record(e.tid, true);
                print("ssh_vt2: recording ON tab=" + e.name + " rc=" + rc);
                sys.notify(rc === term.OK ? "記録を開始しました"
                                          : ("記録できない (" + rc + ")"));
                returnTerminal();
            });
        }
        s.button("このタブを閉じる", function () {
            closeTab(i);
            if (!sessions.length) {
                inForm = false;
                unwind();
                ui.clear(BG);
                hostsPage(null);
                return;
            }
            if (actIdx < 0)
                actIdx = 0;
            returnTerminal();
            switchTo(actIdx);
        });
        s.button("戻る", returnTerminal);
    };

    confirmPaste = function (id, data) {
        inForm = true;
        ui.keyboard(0);
        var s = ui.screen("危険な貼り付け");
        s.label("改行または制御文字を含みます (" + data.length + " 文字)");
        var preview = data.replace(/[\x00-\x1f\x7f]/g, " ");
        if (preview.length > 120)
            preview = preview.slice(0, 120) + "…";
        s.label(preview);
        s.button("送信する", function () {
            ssh.write(id, data);
            returnTerminal();
        });
        s.button("キャンセル", returnTerminal);
    };

    var trustHostPage = function (e, fingerprint, changed) {
        inForm = true;
        ui.keyboard(0);
        var s = ui.screen(changed ? "ホスト鍵が変更されました" : "ホスト鍵を確認");
        s.label(e.host + ":" + e.port);
        s.label("SHA-256: " + fingerprint);
        s.label(changed ? "MITMの可能性があります。確認できるまで信頼しないでください。"
                        : "別経路で指紋を確認してから信頼してください。");
        s.button(changed ? "新しい鍵を信頼して再接続" : "信頼して再接続", function () {
            vault.put(hostKeyName(e), fingerprint);
            unwind();
            startSession(e);
        });
        s.button("キャンセル", function () {
            if (e.transientCred)
                vault.del(credName(e));
            hostsPage("接続を中止しました");
        });
    };

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
        fw.setText("");
        var sv = s.toggle("この接続先を保存", 1);
        var sc = s.toggle("パスワードをVaultに保存", 1);
        var mk = function () {
            var p = parseInt(fp.value(), 10);
            return { host: fh.value(), port: isNaN(p) ? 22 : p,
                     user: fu.value(), password: fw.value() };
        };
        var put = function (e) {
            if (!sv.value())
                return;
            var hosts = loadHosts();
            var saved = { host: e.host, port: e.port, user: e.user };
            if (editIdx >= 0 && editIdx < hosts.length)
                hosts[editIdx] = saved;
            else
                hosts.push(saved);
            saveHosts(hosts);
        };
        s.button("接続", function () {
            var e = mk();
            if (e.password)
                vault.put(credName(e), e.password);
            delete e.password;
            e.transientCred = !sc.value();
            if (!vault.has(credName(e))) {
                sys.notify("パスワードを入力してください");
                return;
            }
            put(e);
            startSession(e);
        });
        if (editIdx >= 0) {
            s.button("保存して一覧へ", function () {
                var e = mk();
                if (e.password)
                    vault.put(credName(e), e.password);
                if (!sc.value())
                    vault.del(credName(e));
                delete e.password;
                put(e);
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
        s.button("接続", function () {
            if (vault.has(credName(e)))
                startSession(e);
            else
                connectForm(e, idx);
        });
        s.button("編集", function () { connectForm(e, idx); });
        s.button("削除", function () {
            var h2 = loadHosts();
            h2.splice(idx, 1);
            saveHosts(h2);
            vault.del(credName(e));
            vault.del(hostKeyName(e));
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
        var dead = 0, i;
        for (i = 0; i < sessions.length; i++)
            if (!sessions[i].live)
                dead++;
        if (sessions.length)
            s.label("タブ: " + sessions.length + " / " + MAX_SESS +
                    " (切断済み " + dead + ")");
        var hosts = loadHosts();
        if (hosts.length) {
            var l = s.list();
            for (i = 0; i < hosts.length; i++) {
                (function (idx, e) {
                    l.add(hostLabel(e), function () { hostActions(idx); });
                })(i, hosts[i]);
            }
        } else {
            s.label("保存済みホストはありません");
        }
        s.button("+ 新規接続", function () {
            connectForm({ host: HOST, port: PORT, user: USER }, -1);
        });
        if (sessions.length) {
            s.button("端末へ戻る", returnTerminal);
        }
        if (dead) {
            s.button("切断済みタブを閉じる", function () {
                for (var k = sessions.length - 1; k >= 0; k--)
                    if (!sessions[k].live)
                        closeTab(k);
                if (sessions.length)
                    returnTerminal();
                else {
                    unwind();
                    hostsPage("閉じました");
                }
            });
        }
    };

    /* ---- ライフサイクル ----------------------------------------------
       画面はアプリ切替で C 側が全破棄する (§3.3)。term は生きたまま
       残るので、背景に回るときは自分の term を隠すこと — レジストリの
       描画は「可視な term」を無条件に blit するので、隠さないと前面の
       アプリのキャンバスに書き込む。 */
    sys.onBackground(function () {
        hideAll();
    });
    sys.onForeground(function () {
        measure();
        if (!inForm && actIdx >= 0 && actIdx < sessions.length) {
            ui.clear(BG);
            repaintActive();
            drawTabs();
            ui.keyboard(2);
        } else {
            inForm = true;
            hostsPage(sessions.length ? "タブは維持されています" : null);
        }
    });

    restoreTabs();
    if (sessions.length) {
        inForm = false;
        ui.clear(BG);
        ui.keyboard(2);
        switchTo(0);
    } else {
        hostsPage(null);
    }
}
