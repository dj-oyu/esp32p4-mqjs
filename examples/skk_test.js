// @app skk_test
// @title SKK入力テスト
// @icon 
// @desc SKK 日本語入力の変換精度と性能を実測する簡易入力ツール。
// @perm ui
/* Tab5 SKK 入力テストツール (docs/skk-ime-design.md の S5 受け皿)。
 *
 * 目的は2つある。ひとつは「変換が当たるか」を人間が目で確かめること、
 * もうひとつは「打鍵と変換がいくら食うか」を遠隔で採ること。前者が画面、
 * 後者が MQTT の proberep で、どちらもファイルには落とさない — 保存機能は
 * 意図的に持たない (メモ帳ではなく計測台なので、電源を切れば消えてよい)。
 *
 * ⚠️ 前提: いま実機の ui.cells に日本語は出ない。端末フォント
 * font_term_mono の かな/カナ/漢字 は 0 グリフで (実測 2026-07-29)、
 * ▽ (U+25BD) も ▼ (U+25BC) も無い。しかも blit_glyph() はグリフ記述子の
 * 取得に失敗すると豆腐ではなく「何も描かずに return」する = 空白セルに
 * なるので、壊れていることが画面から分からない。設計の S3 (cells の
 * 全角2セル対応) と S4 (フォント再生成) が入るまでこれは変わらない。
 *
 * そこでこのツールは桁と描画を分けてある。
 *   - 桁の計算は必ず ui.cells のモデル (ui.cellWidth + CONT セル) で行う。
 *     S3/S4 が来た日にそのまま正しく描けるのが目的で、いま画面に何が
 *     見えているかとは独立に検証できる。
 *   - 実際の描画は行単位で選ぶ: ASCII だけの行は ui.cells、日本語を含む
 *     行は ui.text (font_noto_jp_20_4 = かな87 + カナ90 + 漢字3,517)。
 * F3 で ui.cells 固定に切り替えられる。そのモードで日本語が空白になるのが
 * 現状の正しい挙動で、S3/S4 の進捗はここで見る。
 *
 * ⚠️ IME トグルは設計 §9.3 の2経路がどちらも現状のファームで届かない。
 *   - Ctrl+Space: kbd_core.c が `if (uc == ' ') return 0;` で明示的に捨てる
 *     (NUL が "\x00name" トークンの先頭と衝突するため)。キーイベント自体が
 *     生成されないので、アプリ側でどう書いても拾えない。
 *   - "\x00ime" トークン: key_token() にまだ無い (C 側の追加が要る)。
 * そこで届く経路に割り当ててある。"\x00ime" も受けるので、C 側にトークンが
 * 入った日にこのファイルは 1 行も変えずに設計どおりになる。
 *
 * キー割り当て:
 *   F1 / Ctrl+\ / Ctrl+O / 画面左上タップ … IME オン・オフ
 *   F2 / 画面右上タップ                   … proberep をいま送る
 *   F3 … ui.cells 固定トグル (S3/S4 の進捗確認)
 *   F4 … 操作ヘルプ (SKK は大文字で変換を始める — 知らないと詰む)
 *   F5 … 本文クリア
 *   Space=変換 / x=前候補 / Enter=確定 / C-g=取消 は skk_core が解釈する
 *
 * プローブ: <BASE>/proberep へ 30 秒ごと + オンデマンド + 停止時。
 *   push   : uv run --with cryptography python tools/mqjs_push.py \
 *              192.168.1.2 esp32p4-mqjs/task/u7q3x9f2 examples/skk_test.js
 *   受信   : mosquitto_sub -h 192.168.1.2 -t 'esp32p4-mqjs/task/u7q3x9f2/#' -v
 *   終わったら tools/dev_idle.js を push し直して dev スロットを戻すこと。
 *
 * 検証フラグ (コメントは 1 行に収める — sed で書き換えて push するため):
 *   SELFTEST=true … PC (run_pc) で台本キーを流して結果を print する */
"use strict";
sys.setAppName("skk_test");

var SELFTEST = false; /* PC: 台本キーを流して変換結果と統計を print する */
var DICT = "";        /* skk.open() に渡す辞書パス。"" = プラットフォーム既定 */

var BASE = "esp32p4-mqjs/task/u7q3x9f2"; /* 既存 probe_*.js と同じ dev タスク配下 */
var PUB_MS = 30000;   /* 定期 publish の間隔 */

/* ---- 配色 (ssh_vt と同じ暗背景パレット) ---- */
var BG = 0x0B0E11;
var FG = 0xC9D1D9;
var CURSOR = 0x4FC3F7;
var BAR_BG = 0x1A222C;
var BAR_FG = 0x8B98A5;
var PRE_FG = 0xFFD479;   /* preedit (▽/▼ 付きの未確定文字列) */
var CAND_FG = 0xC9D1D9;
var SEL_BG = 0x2E6BD6;   /* 選択中の候補だけ別 run で反転 (設計 §9.2) */
var SEL_FG = 0xFFFFFF;
var ERR_FG = 0xE05A4E;

/* ---- 計測 (要件: 遠隔からハードウェアリソース消費を確認できること) ----
   JS から取れる単調時計は performance.now() の 1 ms と sys.micros() の
   1 µs の2つしかない。打鍵経路は設計 §4.1 で µs 規模なので ms では 0 と
   しか読めず、ここは sys.micros() を使う。
   ⚠️ sys.micros() は絶対値なので、稼働 17.9 分 (2^30 µs) を超えると
   mquickjs の short int を外れて呼ぶたびにアリーナを確保する。差分を取る
   ぶんには値は正しいが、長時間稼働では計測自体が僅かに重くなる。 */
var HAVE_US = (typeof sys.micros === "function");
var keyN = 0, keySum = 0, keyMin = 0, keyMax = 0;    /* 打鍵→再描画 (µs) */
var convN = 0, convSum = 0, convMin = 0, convMax = 0; /* Space→候補 (µs) */
var convTry = 0, convHit = 0;   /* 変換を試みた / 候補が出た */
var misses = [];                /* 候補が無かった見出しの実物 (数件) */
var recent = [];                /* 直近の 見出し→確定 の対 (精度の目視用) */
var pubs = 0;
var linkUp = false;
var heapBefore = null, heapAfter = null;
var heapNow = [0, 0, 0];   /* sys.heap() のキャッシュ (statusDraw の項を参照) */
var dictLoadUs = 0, dictBytes = 0, idxBytes = 0, entries = 0;

/* ---- IME ハンドル ----
   skk が無いファーム / 辞書が入っていない実機 / run_pc のどれでも
   「構文エラーなくロードできる」ことが要件なので、未定義参照は typeof で
   避け、open は必ず try で囲む (未定義グローバルの直接参照は ReferenceError、
   typeof だけが安全)。 */
var ime = 0;
var imeReady = false;
var imeOn = false;
var imeErr = "";
var cands = [];
var candSel = 0;
var lastReading = "";  /* 直近の変換試行の見出し (確定と対にして記録する) */

/* ---- 入力バッファ ----
   1 要素 = 1 文字 (サロゲートペアは結合済み)。bufW は同じ添字のセル幅で、
   ui.cellWidth() を打鍵ごとに全文へ引き直さないためのキャッシュ。
   再レイアウトが純 JS で済み、C 呼び出しは「入った文字ぶん 1 回」だけになる。 */
var buf = [];
var bufW = [];
var cur = 0;
var BUF_MAX = 1200;   /* 計測台なので上限は小さくてよい。溢れたら頭を捨てる */

/* ---- 画面メトリクス ----
   行割り: 0 = ステータス、1..TROWS = 本文、下から2行目 = IME バー、
   最下行 = ヒント。桁数と行数は必ず ui.size()/ui.cellSize() から導く。 */
var W = 720, H = 1192, KB_H = 480, CW = 9, LH = 24;
var COLS = 80, GRID_ROWS = 29, TROWS = 26, WRAP = 72;
var BODY_TOP = 1, IME_ROW = 27, HINT_ROW = 28;
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
   上描きすると、それが永久に残る。フロート窓とカーソルがまさにそれで、
   カーソルは移動のたびに古い位置へ描いたぶんが消えずに溜まっていた
   (実機報告 2026-07-29: 水色の下線が複数残る)。描いた行をここに積み、
   次の bodyDraw の冒頭で署名を落として引き直させる。 */
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

/* 表示用の見出し。preedit は "▽かんじ" の形で来るのでマーカーを外す。 */
function stripMark(s) {
    if (!s)
        return "";
    var c = s.charCodeAt(0);
    if (c === 0x25BD || c === 0x25BC)
        return s.slice(1);
    return s;
}

function modeName(m) {
    if (m === skk.ASCII) return "ascii";
    if (m === skk.KANA) return "kana";
    if (m === skk.KATA) return "kata";
    if (m === skk.MIDASHI) return "midashi";
    if (m === skk.OKURI) return "okuri";
    if (m === skk.SELECT) return "select";
    return "?";
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
    IME_ROW = GRID_ROWS - 2;
    HINT_ROW = GRID_ROWS - 1;
    TROWS = GRID_ROWS - 3;
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

function bodyDraw(force) {
    /* 前回フロート窓とカーソルが覆った行を汚す。ここでやるので、この後の
       描画で下の本文が戻り、続く imeFloatDraw が新しい位置に描く。 */
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
    }
}

/* ステータス行は ASCII だけで組む (cells で確実に出る唯一の行)。
   ⚠️ ここで sys.heap() を呼んではいけない。3 要素目の LVGL プール残量は
   ui_tab5_lv_mem_free() → lvgl_port_lock() + lv_mem_monitor() で、LVGL の
   ロックを取って tlsf プールを歩く。打鍵ごとに呼ぶと描画タスクの 1 フレーム
   分ブロックしうるうえ、いま測っている打鍵レイテンシそのものを汚す。
   1 秒のティックで採ったキャッシュを表示する。 */
function statusDraw() {
    var h = heapNow;
    var m = imeReady ? (imeOn ? modeName(skk.mode(ime)) : "OFF") : "n/a";
    var s = "SKK:" + m +
        " buf:" + buf.length +
        " ram:" + kbytes(h[0]) + "/" + kbytes(h[1]) + "/" + kbytes(h[2]) +
        " cv:" + convHit + "/" + convTry +
        " k:" + (keyN ? ((keySum / keyN) | 0) : 0) + "u" +
        " c:" + (convN ? ((convSum / convN) | 0) : 0) + "u" +
        " up:" + upSec() + " pub:" + pubs;
    ui.cells(0, 0, pad(s, COLS), BAR_FG, BAR_BG);
}

function hintDraw() {
    var s = "F1/C-\\ IME  F2 send  F3 cells-only  F4 clear  " +
        "tap: TL=IME TR=send";
    ui.cells(0, HINT_ROW, pad(s, COLS), BAR_FG, BAR_BG);
}

/* 下部の状態行。モードと操作ヒントだけ。preedit と候補はカーソル位置の
   フロート窓へ移した (imeFloatDraw)。
   ▽▼ もかなも端末フォントに無いので、この行は常に ui.text で描く
   (CELLS_ONLY でも同じ — cells にすると行ごと空白になって何も見えない)。 */
function imeDraw() {
    var y = IME_ROW * LH;
    ui.rect(0, y, W, LH, BAR_BG);
    if (!imeReady) {
        ui.text(4, y + 1, imeErr ? imeErr : "skk なし", ERR_FG);
        return;
    }
    if (!imeOn) {
        ui.text(4, y + 1, "IME OFF  —  F1 / Ctrl+\\ / 左上タップ", BAR_FG);
        return;
    }
    ui.text(4, y + 1, "かな入力中  —  大文字で変換開始 (F4 で操作ヘルプ)",
            BAR_FG);
}

/* 変換中のフロート窓。preedit と候補をカーソルの位置に重ねて出す。
 *
 * 設計 §9.1 が preedit を専用行に置けと言っているのは ssh_vt の話で、
 * あそこはグリッドをリモートが所有していて勝手に書くと再描画で消え、
 * 選択コピーの座標もずれる。このアプリは本文を自分で持っているので
 * その制約が無く、カーソル位置に出せる — 視線が下端と入力位置を往復
 * しないぶん段違いに読みやすい。
 *
 * 覆った行は floatRows に記録して、次の bodyDraw に引き直させる。 */
function imeFloatDraw() {
    if (!imeReady || !imeOn || helpOn)
        return;
    var pre = skk.preedit(ime);
    var n = cands.length;
    if (!pre && !n)
        return;

    var xy = cursorXY();
    var cr = xy[1];
    if (cr < 0 || cr >= TROWS)
        return;

    /* preedit はカーソルのある行に重ねる。未確定なので本文を隠してよい。 */
    var pw = pre ? ui.textSize(pre)[0] : 0;
    var px = xy[0];
    if (px + pw + 8 > W)
        px = W - pw - 8;
    if (px < 0)
        px = 0;
    if (pre) {
        ui.rect(px - 2, (BODY_TOP + cr) * LH, pw + 8, LH, BAR_BG);
        ui.text(px + 2, (BODY_TOP + cr) * LH + 1, pre, PRE_FG);
        dirtyRows.push(cr);
    }
    if (!n)
        return;

    /* 候補は 1 行下。下端に当たるなら上に返す。 */
    var br = cr + 1;
    if (br >= TROWS)
        br = cr - 1;
    if (br < 0)
        return;

    /* 幅に収まる範囲で、選択位置を中心にウィンドウする (全候補一覧は
       初版では作らない)。まず幅を測ってから箱を描く。 */
    var first = candSel - 2;
    if (first < 0)
        first = 0;
    var wsum = 8;
    var last = first;
    for (var i = first; i < n; i++) {
        var w2 = ui.textSize(cands[i])[0] + 14;
        if (wsum + w2 > W - 8)
            break;
        wsum += w2;
        last = i + 1;
    }
    var bx = px;
    if (bx + wsum > W)
        bx = W - wsum;
    if (bx < 0)
        bx = 0;
    var by = (BODY_TOP + br) * LH;
    ui.rect(bx, by, wsum, LH, BAR_BG);

    var x = bx + 6;
    for (var j = first; j < last; j++) {
        var t = cands[j];
        var tw = ui.textSize(t)[0];
        if (j === candSel) {
            ui.rect(x - 3, by + 1, tw + 6, LH - 2, SEL_BG);
            ui.text(x, by + 1, t, SEL_FG);
        } else {
            ui.text(x, by + 1, t, CAND_FG);
        }
        x += tw + 14;
    }
    dirtyRows.push(br);
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
    "  F1 IME on/off   F2 計測送信   F3 cells 固定   F5 本文クリア",
];

function helpDraw() {
    ui.rect(0, BODY_TOP * LH, W, TROWS * LH, BAR_BG);
    for (var i = 0; i < HELP.length && i < TROWS; i++)
        ui.text(8, (BODY_TOP + i) * LH + 1, HELP[i],
                i === 0 ? PRE_FG : FG);
}

function redrawAll() {
    ui.clear(BG);
    drawn = [];
    dirtyRows = [];
    relayoutText();
    if (helpOn) {
        helpDraw();
    } else {
        bodyDraw(true);
        imeFloatDraw();
    }
    statusDraw();
    imeDraw();
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

/* ---- IME ---- */
function openIme() {
    if (typeof skk === "undefined") {
        imeErr = "skk バインディング無し (ROM 未再生成)";
        return;
    }
    heapBefore = sys.heap();
    var t0 = nowUs();
    try {
        ime = skk.open(DICT);
    } catch (eOpen) {
        imeErr = "skk.open 失敗: " + eOpen;
        heapAfter = heapBefore;
        heapNow = heapBefore;
        return;
    }
    dictLoadUs = (nowUs() - t0) | 0;
    heapAfter = sys.heap();
    heapNow = heapAfter;
    imeReady = true;
    imeOn = true;
    skk.enable(ime, true);
    var st = coreStats();
    if (st) {
        dictBytes = st.dictBytes;
        /* 索引のバイト数。image は「本文 + packed key 8B + オフセット 4B」で
           見出し 1 件あたり 12 B なので、見出し数から逆算できる (設計 §6.4:
           梅で 8,346 件 = 約 100 KB)。C 側が内訳を返さないのでここで組む。 */
        idxBytes = (st.nasi + st.ari) * 12;
        entries = st.nasi + st.ari;
    }
}

function toggleIme() {
    if (!imeReady)
        return;
    imeOn = !imeOn;
    skk.enable(ime, imeOn);
    if (imeOn)
        skk.setMode(ime, skk.KANA);   /* C-j が届かないので明示的に戻す */
    else
        skk.reset(ime);
    cands = [];
    candSel = 0;
    sys.notify(imeOn ? "IME オン (かな)  大文字で変換開始 / F4 ヘルプ"
                     : "IME オフ");
    bodyDraw(false);
    imeFloatDraw();
    imeDraw();
    statusDraw();
}

/* skk.stats() は audio.stats() と同じ「JSON 文字列 1 本」形。C 側で µs を
   積算しているので、JS で performance.now() を挟むより正確 (mquickjs の
   ディスパッチが混ざらない)。 */
function coreStats() {
    if (!imeReady)
        return null;
    try {
        return JSON.parse(skk.stats(ime));
    } catch (eStat) {
        return null;
    }
}

/* proberep 1 通を esp-mqtt の既定バッファ (1024 B) に収めるため、記録する
   文字列は短く切る。日本語は JSON で 1 文字 3 バイトに膨らむ。 */
function clip(s, n) {
    return s.length > n ? s.slice(0, n) + "…" : s;
}

function noteMiss(reading) {
    if (!reading)
        return;
    for (var i = 0; i < misses.length; i++)
        if (misses[i] === reading)
            return;
    if (misses.length < 6)
        misses.push(clip(reading, 12));
}

function noteRecent(reading, out) {
    if (!reading || !out)
        return;
    recent.push({ r: clip(reading, 12), c: clip(out, 12) });
    while (recent.length > 4)
        recent.shift();
}

/* IME へ 1 キー渡す。戻り値 = ステータスビットマスク (0 = 素通し)。
   辞書を引いた打鍵とそうでない打鍵は予算が3桁違う (設計 §4.1: 打鍵 55 ms /
   変換は実質数十 ms) ので、µs は別々のバケツに積む。

   「この打鍵は変換か」の判定が地味に難しい。Space の明示変換だけを数えると
   送りありの「送り仮名が確定した瞬間の自動変換」を丸ごと取りこぼし、C 側の
   lookups と件数が合わなくなる (実測: 台本で lookups 4 に対し 3 しか数え
   られなかった)。なので ▽/▼ を組んでいる最中の打鍵で、Space か、候補が
   出たか、▼ に入ったかのどれかが起きたものを変換として数える。
   候補ゼロの空振りも変換に数える — 数十 µs 使うのに CANDS が立たないので、
   落とすと「引けなかった見出し」が統計から消える。 */
function feedIme(k) {
    var mode = skk.mode(ime);
    var composing = (mode === skk.MIDASHI || mode === skk.OKURI);
    /* 見出しを控えるのは「この打鍵が変換を起こしうる」ときだけ。
       組んでいる最中の全打鍵で preedit を引くと、読みを打つ間ずっと
       1 打鍵あたり JS 文字列を 1 本作ることになる — 設計 §4.2 が
       bitmask で消したはずのコストがアプリ側に戻ってしまう。
       変換が起きうるのは (a) Space の明示変換、(b) 大文字で送り仮名が
       始まる瞬間、(c) OKURI 中の打鍵で送り仮名の頭が確定する瞬間、の
       3 つだけ。読みを打っている間の小文字は 1 本も作らない。
       変換後は preedit が候補 (▼漢字) に変わって読みが読めなくなるので、
       これは呼ぶ前に控えておくしかない。 */
    var mayConvert = composing &&
                     (k === " " || mode === skk.OKURI ||
                      (k.length === 1 && k >= "A" && k <= "Z"));
    var reading = mayConvert ? stripMark(skk.preedit(ime)) : "";
    var t0 = nowUs();
    var st = skk.key(ime, k);
    var dt = (nowUs() - t0) | 0;
    if (composing &&
        ((st & skk.CANDS) || k === " " || skk.mode(ime) === skk.SELECT)) {
        lastReading = reading;
        convTry++;
        if (st & skk.CANDS)
            convHit++;
        else
            noteMiss(reading);
        convN++;
        convSum += dt;
        if (convN === 1 || dt < convMin)
            convMin = dt;
        if (dt > convMax)
            convMax = dt;
    }
    return st;
}

/* ---- キー処理 ---- */
function handleKey(k) {
    var code = k.charCodeAt(0);

    /* レイアウト変更は IME より先。セッションもバッファも関係ない */
    if (code === 0) {
        var name = k.slice(1);
        if (name === "rotate") {
            relayout();
            return;
        }
        /* アプリ自身のホットキーは IME より先に取る。"\x00ime" は設計 §9.3 が
           要求しているトークンだが C 側にまだ無いので、届く F1 を主経路に
           する (制御バーの Fn レイヤ)。 */
        if (name === "ime" || name === "f1") {
            toggleIme();
            return;
        }
        if (name === "f2") {
            publishNow("ondemand");
            sys.notify(linkUp ? "proberep 送信" : "MQTT 未接続");
            return;
        }
        if (name === "f3") {
            CELLS_ONLY = !CELLS_ONLY;
            sys.notify(CELLS_ONLY ? "cells 固定 (日本語は空白が正)"
                                  : "自動 (日本語は ui.text)");
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

    /* ドックの Ctrl+\ (= 0x1C) を IME トグルに割り当てる。設計 §9.3 は
       Ctrl+Space と書いているが、kbd_core は Ctrl+Space を明示的に捨てる
       (NUL が "\x00name" トークンの先頭と衝突するため) ので届かない。
       Ctrl+\ は emacs の toggle-input-method と同じで US 配列にもある。 */
    if (code === 0x1C || code === 0x0F) {   /* Ctrl+\ / Ctrl+O */
        toggleIme();
        return;
    }

    /* IME が消費したらここで終わり (設計 §8 の使い方そのまま)。
       imeOn が false なら skk.key() すら呼ばない = C 関数呼び出し 1 回ぶんの
       オーバーヘッドも出さない (§4.2)。 */
    if (imeOn && imeReady) {
        var st = feedIme(k);
        if (st !== 0) {
            if (st & skk.COMMIT) {
                var out = skk.commit(ime);
                if (out) {
                    insertText(out);
                    noteRecent(lastReading, out);
                    lastReading = "";
                    /* 本文が変わるのは確定したときだけ。ここを IME が
                       消費した全打鍵で回すと、preedit を 1 文字動かす
                       たびに最大 BUF_MAX 要素の折り返し計算をやり直す
                       ことになる (本文は 1 文字も変わっていないのに)。 */
                    relayoutText();
                }
            }
            /* 候補配列を作り直すのは CANDS が立った 1 回だけ (設計 §4.4)。
               ▼ の中を Space/x で移動する連打は SEL しか立たないので、
               整数を 1 つ読むだけで済む — ここで candidates() を毎回
               呼ぶと ▼ 連打のたびに文字列を作り直すことになる。 */
            if (st & skk.CANDS) {
                cands = skk.candidates(ime);
                candSel = skk.sel(ime);
            } else if (st & skk.SEL) {
                candSel = skk.sel(ime);
            } else if (skk.mode(ime) !== skk.SELECT) {
                cands = [];    /* ▼ を抜けた: 候補バーを畳む */
                candSel = 0;
            }
            blinkOn = true;   /* 打鍵中は点灯のまま */
            bodyDraw(false);
            imeFloatDraw();
            imeDraw();
            statusDraw();
            return;
        }
    }

    /* ここから下は IME が触らなかったキー */
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
    imeFloatDraw();
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
        dict: { loadUs: dictLoadUs, bytes: dictBytes, idx: idxBytes,
                keys: entries, ok: imeReady ? 1 : 0 },
        key: { n: keyN, minUs: keyMin, avgUs: avg(keySum, keyN), maxUs: keyMax },
        conv: { n: convN, minUs: convMin, avgUs: avg(convSum, convN),
                maxUs: convMax, tries: convTry, hits: convHit },
        buf: buf.length,
        cols: WRAP,
        rows: TROWS
    };
    var sent = pub(main);
    /* C 側の µs とプローブ数は別メッセージ。JS 側の計測と突き合わせると
       「JS の何割がエンジンか」が出る。 */
    var core = coreStats();
    if (core)
        sent += pub({ phase: phase + "-core", core: core });
    /* 変換精度の実体: 外した見出しと、直近の 見出し→確定 の対 */
    if (misses.length || recent.length)
        sent += pub({ phase: phase + "-acc", miss: misses, recent: recent });
    return sent;
}

/* ---- 起動 ---- */
measure();
openIme();

var HAVE_SCREEN = ui.size()[0] !== 0;
if (HAVE_SCREEN) {
    ui.clear(BG);
    ui.keyboard(2);   /* キーボード + 制御バー (Fn レイヤに F1-F4 がある) */
    redrawAll();
}

ui.onKey(onKeyTimed);

ui.onTouch(function (x, y, kind) {
    if (kind !== 0)
        return;
    if (y < LH) {
        /* ステータス行を操作面に使う: 左 1/3 = IME トグル、右 1/3 = 送信。
           ドックが無いときに F1/F2 の代わりになる唯一の経路。 */
        if (x < W / 3)
            toggleIme();
        else if (x > W * 2 / 3) {
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
   ヘルプ表示中とフロートに覆われている間は動かさない。 */
setInterval(function () {
    if (!HAVE_SCREEN || helpOn)
        return;
    blinkOn = !blinkOn;
    bodyDraw(false);
    imeFloatDraw();
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
   ここを通しておくと「block 内の function 宣言が巻き上がらない」以外の
   ロジックバグは実機へ持って行く前に落ちる (キー経路そのものは PC でも
   同じ関数を通る)。 */
function feedScript(a) {
    for (var i = 0; i < a.length; i++)
        onKeyTimed(a[i]);
}

function selftest() {
    print("skk_test selftest ------------------------------------------");
    print("ime      : " + (imeReady ? "ok" : "NG " + imeErr));
    print("dict     : " + dictBytes + " B / idx " + idxBytes +
          " B / " + entries + " keys / open " + dictLoadUs + " us");

    /* 変換: 送りなし・送りあり・カタカナ・引けない見出し。Enter は ▼ を
       確定するので本文には改行が入らない (SKK としてこれが正しい)。 */
    feedScript(["K", "a", "n", "j", "i", " ", "\n"]);
    feedScript(["O", "k", "u", "R", "u", "\n"]);
    feedScript(["N", "i", "h", "o", "n", "g", "o", " ", "\n"]);
    feedScript(["q", "k", "a", "t", "a", "q"]);         /* カタカナモード */
    feedScript(["Z", "z", "z", "z", "z", "z", " ", "\n"]); /* 引けない見出し */
    print("conv buf : " + JSON.stringify(bufText()));

    /* 素通し経路: IME を落として ASCII と改行と Backspace を通す。 */
    feedScript(["\x00f1"]);
    feedScript(["\n", "a", "b", "c", "!", "\b"]);
    print("ascii buf: " + JSON.stringify(bufText()));

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
    print("conv us  : n=" + convN + " min=" + convMin +
          " avg=" + avg(convSum, convN) + " max=" + convMax +
          " try=" + convTry + " hit=" + convHit);
    print("miss     : " + JSON.stringify(misses));
    print("recent   : " + JSON.stringify(recent));
    print("core     : " + (imeReady ? skk.stats(ime) : "-"));
    print("heap d   : " + (heapBefore && heapAfter
        ? (heapBefore[0] - heapAfter[0]) + " iram / " +
          (heapBefore[1] - heapAfter[1]) + " psram"
        : "-"));
}

if (SELFTEST)
    selftest();
