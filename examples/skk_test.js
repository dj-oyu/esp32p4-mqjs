// @app skk_test
// @title SKK入力テスト
// @icon 
// @desc SKK 日本語入力の変換精度と性能を実測する簡易入力ツール。
// @perm ui
/* Tab5 SKK 入力テストツール (docs/keyboard-ime-unification.md I4)。
 *
 * 目的は2つある。ひとつは「変換が当たるか」を人間が目で確かめること、
 * もうひとつは「打鍵と変換がいくら食うか」を遠隔で採ること。前者が画面、
 * 後者が MQTT の proberep で、どちらもファイルには落とさない — 保存機能は
 * 意図的に持たない (メモ帳ではなく計測台なので、電源を切れば消えてよい)。
 *
 * I4: 日本語入力はプラットフォームの持ち物。このアプリがするのは ui.ime(1)
 * の opt-in と、カーソルが動いたときの ui.caret(x,y,h) だけで、「あ」の
 * トグルも preedit も候補もモード表示も C が持ち、確定した日本語は
 * ui.onKey に**普通の文字列として**届く (ssh_vt.js と同じ形)。
 *
 * ここには以前 skk.* を直接叩く私設セッションがあり、「エンジンの試験台
 * だから」という理由で残していた。それが表示の嘘を1つ作っていた: 制御バーの
 * 「あ」キーの面はプラットフォームのセッションを見て A/あ/ア を出すので、
 * このアプリで日本語を打っている間ずっと **「A」= 英数** と主張していた
 * (実機報告 2026-07-30)。面の側に「どちらとも言えない」4つ目の状態を足す
 * より、例外の側を無くす方が正しい。
 *
 * 計測も一緒に移った。私設セッションで測る µs は、移行後は**誰も通らない道**
 * の値になる — 実際の打鍵は mqjs_post_key → コマンドキュー → 所有タスク →
 * ime_feed → skk_key と流れ、キューの待ちも学習の鍵も私設セッションには
 * 無い。いまはその経路そのものを C 側が測っていて、ui.imeStats() が
 * エンジンのカウンタ・変換の µs・**キューで待った時間 (hop)** を返す。
 * hop はこの構造にしか無い数字で、「打鍵が重いか」の答えはたいていそこに居る。
 *
 * 桁と描画:
 *   - 桁の計算は必ず ui.cells のモデル (ui.cellWidth + CONT セル) で行う。
 *   - 描画は行単位で選ぶ: ASCII だけの行は ui.cells、日本語を含む行は
 *     ui.text (font_noto_jp = かな + 漢字3,517)。F3 で ui.cells 固定。
 * ⚠️ 2026-07-29 の e36ea53 以降、端末フォント font_term_mono にも かな と
 * JIS 第1水準 (2,965 字) が入り、ui.cells は全角を2セルで描く。つまり F3 は
 * 「日本語が空白になる」モードではなく **2つの描画経路の比較** になった。
 * 第2水準と記号の一部は今も端末フォントに無く、blit_glyph は記述子を引け
 * ないと豆腐ではなく無描画 = 空白セルになるので、そこは F3 で見つかる。
 *
 * キー割り当て:
 *   「あ」(制御バー) / ドックの IME キー … 日本語入力オン・オフ (C が処理)
 *   F2 / 画面右上タップ … proberep をいま送る
 *   F3 … ui.cells 固定トグル (2経路の比較)
 *   F4 … 操作ヘルプ (SKK は大文字で変換を始める — 知らないと詰む)
 *   F5 … 本文クリア
 *   Space=変換 / x=前候補 / Enter=確定 / C-g=取消 は skk_core が解釈する
 *   (これらの打鍵はアプリまで降りてこない。ime_feed が先に食う)
 *
 * プローブ: <BASE>/proberep へ 30 秒ごと + オンデマンド + 停止時。
 *   push   : uv run --with cryptography python tools/mqjs_push.py \
 *              192.168.1.2 esp32p4-mqjs/task/u7q3x9f2 examples/skk_test.js
 *   受信   : mosquitto_sub -h 192.168.1.2 -t 'esp32p4-mqjs/task/u7q3x9f2/#' -v
 *   終わったら tools/dev_idle.js を push し直して dev スロットを戻すこと。
 *
 * ⚠️ 日本語入力は**実機でしか試せない**。IME のフックは mqjs_post_key() の
 * #ifdef ESP_PLATFORM の中にあり、run_pc では ui.onKey がそもそも発火しない。
 * エンジンとセッション方針は tools/tests/ime_diff.sh と
 * tools/tests/test_ime_core.c がホストで見ている。
 *
 * 検証フラグ (コメントは 1 行に収める — sed で書き換えて push するため):
 *   SELFTEST=true … PC (run_pc) で台本キーを流して結果を print する */
"use strict";
sys.setAppName("skk_test");

var SELFTEST = false; /* PC: 台本キーを流して描画モデルと統計を print する */

var BASE = "esp32p4-mqjs/task/u7q3x9f2"; /* 既存 probe_*.js と同じ dev タスク配下 */
var PUB_MS = 30000;   /* 定期 publish の間隔 */

/* ---- 配色 (ssh_vt と同じ暗背景パレット) ---- */
var BG = 0x0B0E11;
var FG = 0xC9D1D9;
var CURSOR = 0x4FC3F7;
var BAR_BG = 0x1A222C;
var BAR_FG = 0x8B98A5;
var TITLE_FG = 0xFFD479;   /* ヘルプの見出し */

/* ---- 計測 (要件: 遠隔からハードウェアリソース消費を確認できること) ----
   JS から取れる単調時計は performance.now() の 1 ms と sys.micros() の
   1 µs の2つしかない。ここで測れるのは「アプリが 1 打鍵で使った時間」だけ
   で、ms では 0 としか読めないので sys.micros() を使う。
   ⚠️ sys.micros() は絶対値なので、稼働 17.9 分 (2^30 µs) を超えると
   mquickjs の short int を外れて呼ぶたびにアリーナを確保する。差分を取る
   ぶんには値は正しいが、長時間稼働では計測自体が僅かに重くなる。

   エンジンと打鍵経路そのものの µs はもうここでは測らない (測れない — 変換
   中の打鍵はアプリまで降りてこない)。C 側が出荷する経路を測っていて、
   ui.imeStats() が返す。両者を突き合わせると「1 打鍵のうち JS が何割か」が
   出るのは以前と同じ。 */
var HAVE_US = (typeof sys.micros === "function");
var HAVE_IMESTATS = (typeof ui.imeStats === "function");
var keyN = 0, keySum = 0, keyMin = 0, keyMax = 0;  /* アプリに届いた打鍵 (µs) */
var pubs = 0;
var linkUp = false;
var heapBefore = null, heapAfter = null;
var heapNow = [0, 0, 0];   /* sys.heap() のキャッシュ (statusDraw の項を参照) */
var imeOk = false;         /* ui.ime(1) が通ったか (= 辞書のあるファーム) */
var core = null;           /* ui.imeStats() のキャッシュ。1 秒ティックで更新 */

/* ---- 入力バッファ ----
   1 要素 = 1 文字 (サロゲートペアは結合済み)。bufW は同じ添字のセル幅で、
   ui.cellWidth() を打鍵ごとに全文へ引き直さないためのキャッシュ。
   再レイアウトが純 JS で済み、C 呼び出しは「入った文字ぶん 1 回」だけになる。 */
var buf = [];
var bufW = [];
var cur = 0;
var BUF_MAX = 1200;   /* 計測台なので上限は小さくてよい。溢れたら頭を捨てる */

/* ---- 画面メトリクス ----
   行割り: 0 = ステータス、1..TROWS = 本文、最下行 = ヒント。
   桁数と行数は必ず ui.size()/ui.cellSize() から導く。
   以前あった「IME バー」の行は消えた: かな/カナ/英数 は制御バーの「あ」
   キーの面が出し、変換中は C のフロート窓が出す (設計 §6)。アプリの画面に
   モード表示を置くと、同じ状態を語る場所が2つになって必ずずれる。 */
var W = 720, H = 1192, KB_H = 480, CW = 9, LH = 24;
var COLS = 80, GRID_ROWS = 29, TROWS = 27, WRAP = 72;
var BODY_TOP = 1, HINT_ROW = 28;
var TXT_A = 10, TXT_F = 20;   /* ui.text の実測幅: 半角 / 全角 */
var BLANK = "";
var CELLS_ONLY = false;       /* F3: ui.cells だけで描く (S3/S4 の進捗確認) */

/* 可視行。vRows は ui.cells 用 (CONT セルを空白で埋めた「1コードポイント =
   1セル」の文字列)、vText は ui.text 用 (実文字のみ)、vAscii はどちらで
   描くかの判定。 */
var vRows = [];
var vText = [];
var vAscii = [];
var curRow = 0, curCol = 0, curPrefix = "";
var top = 0;
var helpOn = false;   /* F4: 操作ヘルプのオーバーレイ */
/* 差分描画をすり抜けて残る上描きを消すための「次回引き直す行」リスト。
   bodyDraw の署名は行の内容だけを見るので、内容が変わらない行に何かを
   上描きすると、それが永久に残る。カーソルがまさにそれで、移動のたびに
   古い位置へ描いたぶんが消えずに溜まっていた (実機報告 2026-07-29)。
   フロート窓は ui.overlay に移したのでもうここには載らない — 残って
   いるのはキャンバスに直接描くカーソルだけ。 */
var dirtyRows = [];
/* カーソルの点滅。打鍵中は必ず表示 (点滅で見失わないため)。 */
var blinkOn = true;
var BLINK_MS = 530;
var drawn = [];   /* 直近に描いた各行の署名。差分描画で cmd キューを守る */

/* ---- 小物 ---- */
function pad(s, n) {
    if (s.length > n)
        return s.slice(0, n);
    while (s.length < n)
        s += " ";
    return s;
}

function kbytes(n) {
    return ((n / 1024) | 0) + "k";
}

function upSec() {
    return (performance.now() / 1000) | 0;
}

function nowUs() {
    return HAVE_US ? sys.micros() : performance.now() * 1000;
}

/* 文字列をコードポイント単位に割る。mquickjs の文字列は UTF-16 なので
   BMP 外 (絵文字) はサロゲートペアで来る。for-of は配列専用で文字列に
   使えないため手で回す。 */
function cpSplit(s) {
    var out = [];
    for (var i = 0; i < s.length; i++) {
        var c = s.charCodeAt(i);
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < s.length) {
            out.push(s.slice(i, i + 2));
            i++;
        } else {
            out.push(s.charAt(i));
        }
    }
    return out;
}

/* ---- レイアウト ---- */
/* 負数 = 表示を変えない純クエリ。0 は「キーボードが無い」ではなく
   「キャンバス未生成でまだ答えられない」なので、そのまま信じて行を
   敷くとキーの裏に潜り込む。直前の値を保つ (ssh_vt と同じ理由)。 */
function currentKb() {
    var v = ui.keyboard(-2);
    return v > 0 ? v : KB_H;
}

function measure() {
    var s = ui.size();
    W = s[0] || W;
    H = s[1] || H;
    var c = ui.cellSize();
    CW = c[0] || CW;
    LH = c[1] || LH;
    KB_H = currentKb();
    GRID_ROWS = ((H - KB_H) / LH) | 0;
    if (GRID_ROWS < 4)
        GRID_ROWS = 4;
    BODY_TOP = 1;
    HINT_ROW = GRID_ROWS - 1;
    TROWS = GRID_ROWS - 2;
    if (TROWS < 1)
        TROWS = 1;
    COLS = (W / CW) | 0;
    if (COLS < 8)
        COLS = 8;
    /* ui.text の実寸を測る。cells は 1 セル 9px の等幅だが ui.text は
       Noto 20px で半角 ~10px / 全角 ~20px と広い。同じ論理行を両方の
       経路で描く以上、折り返し幅は「広いほう」で決めないと日本語の行だけ
       画面からはみ出す。10 文字ぶんまとめて測って割るのは、1 文字だと
       字送りの丸めが効いて誤差が出るため。 */
    var aw = ui.textSize("MMMMMMMMMM")[0] / 10;
    var fw = ui.textSize("ああああああああああ")[0] / 10;
    TXT_A = aw > 0 ? aw : CW;
    TXT_F = fw > 0 ? fw : CW * 2;
    var unit = TXT_A > TXT_F / 2 ? TXT_A : TXT_F / 2;
    var wrapText = unit > 0 ? (W / unit) | 0 : COLS;
    WRAP = COLS < wrapText ? COLS : wrapText;
    if (WRAP < 8)
        WRAP = 8;
    BLANK = " ".repeat(WRAP);
    drawn = [];   /* 桁数が変わった: 差分描画の記憶を捨てる */
}

/* 論理バッファを可視行へ流し込む。ここが設計 §9.4 / S3 の桁計算そのもので、
   全角は ui.cellWidth() が 2 を返すのでセルを 2 つ使い、後半には CONT
   (描画時は空白 1 コードポイント) を置く。ui.cells は「コードポイント 1 個
   = セル 1 個」なので、これを入れないと全角の後ろが 1 桁ずつ詰まる。 */
function relayoutText() {
    vRows = [];
    vText = [];
    vAscii = [];
    var cells = "";
    var text = "";
    var col = 0;
    var ascii = true;
    var flushRow = function () {
        while (col < WRAP) {
            cells += " ";
            col++;
        }
        vRows.push(cells);
        vText.push(text);
        vAscii.push(ascii);
        cells = "";
        text = "";
        col = 0;
        ascii = true;
    };
    curRow = 0;
    curCol = 0;
    curPrefix = "";
    for (var i = 0; i <= buf.length; i++) {
        if (col >= WRAP)
            flushRow();
        var ch = i < buf.length ? buf[i] : null;
        var w = 1;
        if (ch !== null && ch !== "\n") {
            w = bufW[i];
            /* 幅2が行末に跨がらないよう先に折る (ssh_vt の feed() と同じ) */
            if (col + w > WRAP)
                flushRow();
        }
        if (i === cur) {
            curRow = vRows.length;
            curCol = col;
            curPrefix = text;
        }
        if (ch === null)
            break;
        if (ch === "\n") {
            flushRow();
            continue;
        }
        cells += ch;
        text += ch;
        if (w === 2)
            cells += " ";   /* CONT: 桁だけ潰す後半セル */
        var cc = ch.charCodeAt(0);
        if (cc < 0x20 || cc > 0x7E)
            ascii = false;
        col += w;
    }
    flushRow();
}

/* ---- 描画 ---- */
/* カーソルの画面座標 [x, 画面行]。本文が cells 経路か text 経路かで x の
   求め方が違う — cells は等幅なので桁 × セル幅、ui.text は可変幅なので
   行頭からの実測幅で置く。フロート窓もカーソル下線も同じ答えが要る。 */
function cursorXY() {
    var useCells = CELLS_ONLY ||
        (curRow < vAscii.length ? vAscii[curRow] : true);
    var cx = curCol * CW;
    if (!useCells)
        cx = curPrefix ? ui.textSize(curPrefix)[0] : 0;
    if (cx > W - CW)
        cx = W - CW;
    return [cx, curRow - top];
}

/* 変換中のフロートを出す位置。C が唯一知り得ない値なので JS が渡す
   (フロートは「目線を動かさない」ために在るので、固定位置に出したら本末
   転倒)。動いたときだけ投げる — 点滅タイマーが毎秒2回 bodyDraw を呼ぶので、
   無条件に呼ぶと何も起きていないのに ui コマンドが積まれ続ける。 */
var caretX = -1, caretY = -1;
function reportCaret(xy) {
    if (typeof ui.caret !== "function")
        return;
    var y = (BODY_TOP + xy[1]) * LH;
    if (xy[0] === caretX && y === caretY)
        return;
    caretX = xy[0];
    caretY = y;
    ui.caret(caretX, caretY, LH);
}

function bodyDraw(force) {
    /* 前回カーソルが覆った行を汚す。この後の描画で下の本文が戻る。 */
    for (var fi = 0; fi < dirtyRows.length; fi++)
        drawn[dirtyRows[fi]] = null;
    dirtyRows = [];

    /* カーソルが見える位置までウィンドウを寄せる */
    if (curRow < top)
        top = curRow;
    if (curRow >= top + TROWS)
        top = curRow - TROWS + 1;
    if (top < 0)
        top = 0;
    for (var r = 0; r < TROWS; r++) {
        var vi = top + r;
        var cellsStr = vi < vRows.length ? vRows[vi] : BLANK;
        var txt = vi < vText.length ? vText[vi] : "";
        var useCells = CELLS_ONLY || (vi < vAscii.length ? vAscii[vi] : true);
        /* 署名 = 描画経路 + 内容。どちらが変わっても引き直す */
        var sig = (useCells ? "c" : "t") + cellsStr;
        if (!force && drawn[r] === sig)
            continue;
        var wasText = !force && drawn[r] && drawn[r].charAt(0) === "t";
        drawn[r] = sig;
        var y = (BODY_TOP + r) * LH;
        if (useCells) {
            /* ui.cells は WRAP 桁ぶんしか塗らないので、ui.text で右まで
               描いてあった行から戻るときは先に行全体を消す */
            if (wasText)
                ui.rect(0, y, W, LH, BG);
            ui.cells(0, BODY_TOP + r, cellsStr, FG, BG);
        } else {
            ui.rect(0, y, W, LH, BG);
            ui.text(0, y + 1, txt, FG);
        }
    }
    /* カーソル (アンダーライン)。cells 経路なら桁 × セル幅で厳密に出るが、
       ui.text 経路は等幅ではないので行頭からの実測幅で置く。 */
    /* 縦棒。描いた行は必ず dirtyRows に積む — 積み忘れると、内容の
       変わらない行に残った棒が次の描画で消えずに増えていく。 */
    var xy = cursorXY();
    if (xy[1] >= 0 && xy[1] < TROWS) {
        if (blinkOn)
            ui.rect(xy[0], (BODY_TOP + xy[1]) * LH + 2, 2, LH - 4, CURSOR);
        dirtyRows.push(xy[1]);
        reportCaret(xy);   /* 描き終えた = カーソルが本当に居る場所が確定 */
    }
}

/* ステータス行は ASCII だけで組む (cells で確実に出る唯一の行)。
   ⚠️ ここで sys.heap() を呼んではいけない。3 要素目の LVGL プール残量は
   ui_tab5_lv_mem_free() → lvgl_port_lock() + lv_mem_monitor() で、LVGL の
   ロックを取って tlsf プールを歩く。打鍵ごとに呼ぶと描画タスクの 1 フレーム
   分ブロックしうるうえ、いま測っている打鍵レイテンシそのものを汚す。
   1 秒のティックで採ったキャッシュを表示する。
   モード (かな/カナ/英数) はここに出さない。出す場所は制御バーの「あ」
   キーの面で、C がそれを持っている (§6.2)。
   エンジン側の数字も同じキャッシュから出す — ui.imeStats() は所有タスクへの
   往復なので、打鍵のたびに呼ぶものではない。 */
function statusDraw() {
    var h = heapNow;
    var s = "buf:" + buf.length +
        " ram:" + kbytes(h[0]) + "/" + kbytes(h[1]) + "/" + kbytes(h[2]) +
        " lk:" + (core ? core.lookups : 0) +
        " cm:" + (core ? core.commits : 0) +
        " c:" + (core && core.convCalls
                    ? ((core.convUs / core.convCalls) | 0) : 0) + "u" +
        " q:" + (core && core.hopCalls
                    ? ((core.hopUs / core.hopCalls) | 0) : 0) + "u" +
        " k:" + (keyN ? ((keySum / keyN) | 0) : 0) + "u" +
        " up:" + upSec() + " pub:" + pubs;
    ui.cells(0, 0, pad(s, COLS), BAR_FG, BAR_BG);
}

/* ヒント行は ASCII だけで組む。全角を混ぜるなら CONT セルを自分で入れる
   必要があり (ui.cells は 1 コードポイント = 1 セル)、忘れると C 側が
   「CONT filler を落とした」と警告しながらグリフを切る。 */
function hintDraw() {
    var s = "IME: kana key   F2 send  F3 cells-only  F4 help  F5 clear  " +
        "tap: TR=send";
    ui.cells(0, HINT_ROW, pad(s, COLS), BAR_FG, BAR_BG);
}

/* F4 の操作ヘルプ。SKK は「大文字で変換を始める」を知らないと一文字も
   漢字にできないのに、その手掛かりが画面のどこにも無かった。 */
var HELP = [
    "SKK 操作ヘルプ            F4 で閉じる",
    "",
    "  変換は大文字で始める",
    "    Aa k a n j i        → ▽かんじ",
    "    Space               → ▼漢字   (もう一度で次候補)",
    "    x                   前の候補",
    "    Enter               確定",
    "    C-g                 取消",
    "    TAB                 読みの補完 (連打で巡回)",
    "",
    "  送り仮名は 2 つ目の大文字で始める",
    "    Aa m o t Aa t e     → 持って",
    "    Aa t a b e Aa r u   → 食べる",
    "",
    "  q   ▽ 中: カタカナで確定 / 通常: かな ⇄ カナ",
    "  Q   空の ▽ を開く",
    "  l   ASCII モードへ抜ける",
    "",
    "  ※ ドックの大文字は Aa を 1 回タップ (次の 1 キーだけ)",
    "     ダブルタップでロック",
    "",
    "  日本語入力の on/off は制御バーの「あ」キー (面が今のモード)",
    "  F2 計測送信   F3 cells 固定   F5 本文クリア",
];

function helpDraw() {
    ui.rect(0, BODY_TOP * LH, W, TROWS * LH, BAR_BG);
    for (var i = 0; i < HELP.length && i < TROWS; i++)
        ui.text(8, (BODY_TOP + i) * LH + 1, HELP[i],
                i === 0 ? TITLE_FG : FG);
}

function redrawAll() {
    ui.clear(BG);
    drawn = [];
    dirtyRows = [];
    relayoutText();
    if (helpOn)
        helpDraw();
    else
        bodyDraw(true);
    statusDraw();
    hintDraw();
}

function relayout() {
    measure();
    redrawAll();
}

/* ---- 編集 ---- */
function insertText(s) {
    var cps = cpSplit(s);
    for (var i = 0; i < cps.length; i++) {
        if (buf.length >= BUF_MAX) {
            /* 溢れたら頭をまとめて捨てる (計測台なので履歴に価値はない) */
            buf.splice(0, 200);
            bufW.splice(0, 200);
            cur -= 200;
            if (cur < 0)
                cur = 0;
        }
        var ch = cps[i];
        var w = 1;
        if (ch !== "\n") {
            w = ui.cellWidth(ch.codePointAt(0));
            if (w !== 2)
                w = 1;   /* 0 (結合文字/VS16) もこのモデルでは 1 セル扱い */
        }
        buf.splice(cur, 0, ch);
        bufW.splice(cur, 0, w);
        cur++;
    }
}

function backspace() {
    if (cur <= 0)
        return;
    buf.splice(cur - 1, 1);
    bufW.splice(cur - 1, 1);
    cur--;
}

function clearBuf() {
    buf = [];
    bufW = [];
    cur = 0;
    top = 0;
}

function bufText() {
    return buf.join("");
}

/* ---- IME ----
   opt-in はこの 1 行きり。あとは C がやる: 「あ」のトグルも、preedit と候補の
   フロートも、モード表示 (制御バーの「あ」キーの面) も、学習の書き戻しも。
   確定した日本語は ui.onKey に普通の文字列として届く。
   ヒープを前後で撮るのは、辞書イメージと索引が実際に食った量が sys.heap()
   の差でしか測れないため (largest contiguous は JS に出ていない)。
   ⚠️ 差が 0 になることがある。プラットフォームのセッションは device で 1 つ
   なので、先に ssh_vt や "ja" の field が arm 済みなら辞書はもう載っていて、
   このアプリは 1 バイトも払わない。0 は「辞書がタダ」ではない。 */
function armIme() {
    heapBefore = sys.heap();
    imeOk = ui.ime(1);   /* false = 辞書の無いファーム */
    heapAfter = sys.heap();
    heapNow = heapAfter;
    core = imeStats();
}

/* 出荷する打鍵経路の実測。私設セッションを開いて測っていた頃の数字は、
   移行後は誰も通らない道の値になるので C 側へ移した (ファイル冒頭)。 */
function imeStats() {
    if (!HAVE_IMESTATS)
        return null;
    return ui.imeStats();   /* null = 一度も arm されていない */
}

/* ---- キー処理 ---- */
function handleKey(k) {
    var code = k.charCodeAt(0);

    /* レイアウト変更はバッファに関係なく先に処理する */
    if (code === 0) {
        var name = k.slice(1);
        if (name === "rotate") {
            relayout();
            return;
        }
        if (name === "f2") {
            publishNow("ondemand");
            sys.notify(linkUp ? "proberep 送信" : "MQTT 未接続");
            return;
        }
        if (name === "f3") {
            CELLS_ONLY = !CELLS_ONLY;
            sys.notify(CELLS_ONLY ? "cells 固定 (端末フォント。第2水準は空白)"
                                  : "自動 (日本語の行は ui.text)");
            redrawAll();
            return;
        }
        if (name === "f4") {
            helpOn = !helpOn;
            redrawAll();
            return;
        }
        if (name === "f5") {
            clearBuf();
            redrawAll();
            return;
        }
    }

    /* ここへ来る打鍵は既に IME を通り抜けたもの — 素通しのキーか、確定した
       日本語の文字列。変換中に飲まれる打鍵 (読みの小文字、Space、Enter、
       ▽ が開いている間の矢印と ESC) はそもそも届かないし、「あ」の
       "\x00ime" も C が食う。順序を間違えようがないのがこの層の目的。 */
    if (code === 0) {
        var tok = k.slice(1);
        if (tok === "left") {
            if (cur > 0)
                cur--;
        } else if (tok === "right") {
            if (cur < buf.length)
                cur++;
        } else if (tok === "home") {
            cur = 0;
        } else if (tok === "end") {
            cur = buf.length;
        } else if (tok === "del") {
            if (cur < buf.length) {
                buf.splice(cur, 1);
                bufW.splice(cur, 1);
            }
        } else {
            return;   /* 知らないトークンは無視 (描き直しもしない) */
        }
    } else if (k === "\b") {
        backspace();
    } else if (k === "\n") {
        insertText("\n");
    } else if (k === "\t") {
        insertText("    ");
    } else if (code === 27) {
        return;   /* Alt 修飾。このツールでは使わない */
    } else if (code < 32) {
        return;   /* Ctrl 合成済みバイト。上で拾わなかったものは捨てる */
    } else {
        insertText(k);
    }
    relayoutText();
    blinkOn = true;   /* 打鍵中は点灯のまま */
    bodyDraw(false);
    statusDraw();
}

/* 打鍵→再描画のレイテンシはここで測る。handleKey の中には ui.* の呼び出しが
   入っているが、ui.* はコマンドをキューへ積むだけで実際の描画は UI タスクが
   やるので、ここで見えるのは「JS 側が 1 打鍵で使った時間」— それが全アプリを
   止める量そのものなので、測りたいのはこちら。 */
function onKeyTimed(k) {
    var t0 = nowUs();
    handleKey(k);
    var dt = (nowUs() - t0) | 0;
    if (dt < 0)
        return;
    keyN++;
    keySum += dt;
    if (keyN === 1 || dt < keyMin)
        keyMin = dt;
    if (dt > keyMax)
        keyMax = dt;
}

/* ---- プローブ (MQTT) ----
   ⚠️ JS の mqtt は自動接続されない。ステータスバーが「MQTT 接続済み」でも
   それはタスク配信クライアントで、アプリの mqtt は別物。
   net.onReady(token => mqtt.connect(token)) を通さないと onConnect は永久に
   発火せず、このアプリは無言で何も publish しない。 */
function pub(o) {
    if (!linkUp)
        return 0;
    try {
        mqtt.publish(BASE + "/proberep", JSON.stringify(o), 0, 0);
    } catch (ePub) {
        linkUp = false;   /* 切れた: 次の onConnect まで黙る */
        return 0;
    }
    pubs++;
    return 1;
}

function avg(sum, n) {
    return n ? ((sum / n) | 0) : 0;
}

/* 1 メッセージは esp-mqtt の既定バッファ (1024 B) に収める。長い集計は
   phase を分けて複数回に割る — 既存 probe_*.js と同じ作法。 */
function publishNow(phase) {
    if (!linkUp)
        return 0;
    heapNow = sys.heap();
    var h = heapNow;
    /* IME 起動前後の差分 = 辞書イメージと索引が実際に食った量。sys.heap() は
       空き合計しか返さないので、両側で撮った差がいちばん確かな実測になる
       (largest contiguous は JS に出ていない)。 */
    var dInt = heapBefore && heapAfter ? heapBefore[0] - heapAfter[0] : 0;
    var dPs = heapBefore && heapAfter ? heapBefore[1] - heapAfter[1] : 0;
    var dLv = heapBefore && heapAfter ? heapBefore[2] - heapAfter[2] : 0;
    var main = {
        phase: phase,
        up: upSec(),
        pub: pubs,
        heap: { iram: h[0], psram: h[1], lvgl: h[2] },
        imeHeap: { iram: dInt, psram: dPs, lvgl: dLv },
        ime: imeOk ? 1 : 0,
        /* アプリ側の打鍵レイテンシ。届くのは IME を通り抜けた打鍵だけなので、
           変換中の打鍵はここに入らない (それは core の conv が持っている)。 */
        key: { n: keyN, minUs: keyMin, avgUs: avg(keySum, keyN), maxUs: keyMax },
        buf: buf.length,
        cols: WRAP,
        rows: TROWS
    };
    var sent = pub(main);
    /* 出荷する打鍵経路の実測は別メッセージ。JS 側の key と突き合わせると
       「1 打鍵のうち JS が何割か」が出る。JSON 文字列ではなくオブジェクトで
       返ってくるので、そのまま入れ子にして流せる。 */
    core = imeStats();
    if (core)
        sent += pub({ phase: phase + "-core", core: core });
    return sent;
}

/* ---- 起動 ---- */
measure();
armIme();

var HAVE_SCREEN = ui.size()[0] !== 0;
if (HAVE_SCREEN) {
    ui.clear(BG);
    ui.keyboard(2);   /* キーボード + 制御バー (「あ」と Fn レイヤの F2-F5) */
    redrawAll();
}

ui.onKey(onKeyTimed);

ui.onTouch(function (x, y, kind) {
    if (kind !== 0)
        return;
    if (y < LH) {
        /* ステータス行の右 1/3 = 計測送信。ドックが無いときに F2 の代わりに
           なる唯一の経路。左 1/3 にあった IME トグルは無くなった — 切り替え
           るものは制御バーの「あ」キーで、そこが状態表示も兼ねている。 */
        if (x > W * 2 / 3) {
            publishNow("ondemand");
            sys.notify(linkUp ? "proberep 送信" : "MQTT 未接続");
            statusDraw();
        }
        return;
    }
    if (y >= GRID_ROWS * LH)
        return;   /* 予約領域 (キーボード / 制御バー) のタップは拾わない */
    ui.keyboard(2);   /* 本文タップ = キーボード呼び戻し */
});

/* レイアウト追従は "\x00rotate" だけに頼らない。トークンはフォアグラウンドの
   アプリにしか届かず、取りこぼした一回がそのままズレっぱなしになる。
   ui.size() と ui.keyboard(-n) は副作用の無いクエリなので突き合わせてよい。
   ついでにステータス行の稼働時間とヒープもここで更新する。 */
setInterval(function () {
    heapNow = sys.heap();   /* 打鍵経路から追い出した唯一の重いクエリ */
    core = imeStats();      /* 同じ理由でここ: IME 所有タスクへの往復 */
    var s = ui.size();
    if (s[0] !== W || s[1] !== H || currentKb() !== KB_H) {
        relayout();
        return;
    }
    if (HAVE_SCREEN)
        statusDraw();
}, 1000);

setInterval(function () { publishNow("tick"); }, PUB_MS);

/* カーソルの点滅。棒を消すには下の行を引き直すしかない (キャンバスに直接
   描いているので「棒だけ消す」と下の文字も削れる) ので、bodyDraw の差分
   描画に任せる — dirtyRows のおかげで引き直るのはカーソルのいた 1 行だけ。
   ヘルプ表示中は動かさない。 */
setInterval(function () {
    if (!HAVE_SCREEN || helpOn)
        return;
    blinkOn = !blinkOn;
    bodyDraw(false);
}, BLINK_MS);

net.onReady(function (token) {
    mqtt.onConnect(function () {
        linkUp = true;
        publishNow("boot");
    });
    mqtt.connect(token);
});

/* 停止直前に最後の 1 通。5 s ウォッチドッグの中で走るので publish だけ。 */
sys.onStop(function (reason) {
    publishNow("stop:" + reason);
});

/* 背面に回ると C 側が画面を全破棄するので、復帰時に敷き直す。 */
sys.onForeground(function () {
    measure();
    redrawAll();
    ui.keyboard(2);
});

/* ---- run_pc 用のセルフテスト ----
   ui.onKey は PC では絶対に発火しないので、台本を直接ハンドラへ流す。
   ⚠️ ここに流せるのは**アプリまで降りてくる打鍵**だけになった。変換は
   mqjs_post_key() の中 (#ifdef ESP_PLATFORM) で起きるので、run_pc に
   "K","a","n","j","i"," " を流しても「かんじ」にはならず ASCII が 6 文字
   入るだけ — 変換を模した台本を置くと、通っていない道を通ったつもりに
   なる。エンジンとセッション方針はホストの ime_diff.sh と test_ime_core.c
   が見ているので、ここで見るのは**このアプリが持っている部分** =
   桁のモデル (CONT セル)、折り返し、カーソル、素通し経路。
   確定した日本語は「普通の文字列」として届くので、そこは実物と同じ形で
   流せる (下の insertText 経由の行)。 */
function feedScript(a) {
    for (var i = 0; i < a.length; i++)
        onKeyTimed(a[i]);
}

function selftest() {
    print("skk_test selftest ------------------------------------------");
    print("ime      : " + (imeOk ? "armed" : "not armed (辞書なし?)"));
    print("stats    : " + (core ? JSON.stringify(core) : "-"));

    /* 素通し経路: ASCII と改行と Backspace。 */
    feedScript(["\n", "a", "b", "c", "!", "\b"]);
    print("ascii buf: " + JSON.stringify(bufText()));

    /* 確定した日本語の受け口。実機ではこの形で ui.onKey に届く
       (1 打鍵 = 1 文字列、複数コードポイントもあり得る)。 */
    feedScript(["\x00f5"]);   /* clear */
    feedScript(["漢字", "を", "書く"]);
    print("commit   : " + JSON.stringify(bufText()) + " cur=" + cur);

    /* 桁詰めと折り返し: 全角だけを WRAP + 4 セルぶん流し込むと、
       CONT が正しければ ちょうど 2 行になり、1 行目の描画文字列は
       「全角 + 空白」の繰り返しで長さが WRAP になる。 */
    feedScript(["\x00f5"]);   /* clear */
    var full = [];
    var nFull = (WRAP / 2 | 0) + 2;
    for (var i = 0; i < nFull; i++)
        full.push("あ");
    feedScript(full);
    var r0 = vRows[0];
    var okLen = r0.length === WRAP;
    var okCont = r0.charAt(0) === "あ" && r0.charAt(1) === " " &&
                 r0.charAt(2) === "あ";
    print("wrap     : wrap=" + WRAP + " rows=" + vRows.length +
          " row0len=" + r0.length + (okLen ? " OK" : " NG") +
          " cont=" + (okCont ? "OK" : "NG") +
          " cur=" + curRow + "," + curCol);

    /* カーソル移動と行頭/行末。left を 3 回打って backspace が
       「カーソルの手前」を消すことを見る。 */
    feedScript(["\x00left", "\x00left", "\x00left", "\b"]);
    print("cursor   : len=" + buf.length + " cur=" + cur +
          " row=" + curRow + "," + curCol);

    print("key us   : n=" + keyN + " min=" + keyMin +
          " avg=" + avg(keySum, keyN) + " max=" + keyMax);
    print("heap d   : " + (heapBefore && heapAfter
        ? (heapBefore[0] - heapAfter[0]) + " iram / " +
          (heapBefore[1] - heapAfter[1]) + " psram"
        : "-"));
}

if (SELFTEST)
    selftest();
