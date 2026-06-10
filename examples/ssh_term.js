/* Tab5 SSH ターミナル (Phase T1): オンスクリーンキーボード + ssh.* で
 * 本物の SSH シェルに繋ぐ最小ターミナル。サーバの出力をコンソール風に
 * 流し、キーボード入力をそのまま送る。VT エスケープの厳密な解釈はまだ
 * せず、印字可能文字と改行・BS だけ扱う (T2 で拡張)。
 *
 * 接続先は下の HOST/USER/PASS を自分の環境に書き換えること。
 * PC では ssh.* はスタブ (接続しない)。 */
"use strict";

var HOST = "192.168.1.10";
var PORT = 22;
var USER = "pi";
var PASS = "changeme";

var sz = ui.size();
var W = sz[0], H = sz[1];
if (!W)
    print("ui: この機体に画面はありません (PC スタブ)");

var BG = 0x0B0E11;
var FG = 0xC9D1D9;
var cell = ui.textSize("M");      /* 等幅近似: 半角 1 マス */
var CW = cell[0], LH = cell[1] + 2;
var KB_H = 400;
var VIEW_H = H - KB_H;
/* wolfSSH の pty は組込み設定では 80x24 固定 (ライブリサイズ不可、T3 で対応)。
 * サーバの stty とずれないよう端末側もこれに合わせる。 */
var COLS = 80;
var ROWS = 24;

/* 行バッファ (素朴な端末: 制御は \n \r \b と印字文字のみ) */
var lines = [""];
var dirty = true;

function putChar(c) {
    var code = c.charCodeAt(0);
    if (c === "\n") {
        lines.push("");
    } else if (c === "\r") {
        /* キャリッジリターン: 現在行頭へ (上書きは簡略化で無視) */
    } else if (code === 8 || code === 127) { /* BS / DEL */
        var ln = lines[lines.length - 1];
        if (ln.length)
            lines[lines.length - 1] = ln.slice(0, -1);
    } else if (code === 27) {
        /* ESC シーケンス: T2 までは無視 (次の 1〜2 文字も捨てない簡易版) */
    } else if (code >= 32) {
        lines[lines.length - 1] += c;
    }
    if (lines.length > ROWS + 200)
        lines.splice(0, lines.length - (ROWS + 200));
    dirty = true;
}

function feed(s) {
    for (var i = 0; i < s.length; i++)
        putChar(s[i]);
}

function redraw() {
    if (!dirty)
        return;
    dirty = false;
    ui.rect(0, 0, W, VIEW_H, BG);
    var start = lines.length > ROWS ? lines.length - ROWS : 0;
    for (var i = start; i < lines.length; i++)
        ui.text(2, (i - start) * LH + 2, lines[i], FG);
}

ssh.onData(function (chunk) {
    feed(chunk);
    redraw();
});

ssh.onClose(function (reason) {
    feed("\n*** SSH closed: " + reason + " ***\n");
    redraw();
    ui.keyboard(0);
});

ui.onKey(function (k) {
    /* キーはそのままサーバへ。Enter は CR (端末の慣習) で送る */
    if (k === "\n")
        ssh.write("\r");
    else if (k === "\b")
        ssh.write("\x7f"); /* DEL: bash の行編集が期待するバックスペース */
    else
        ssh.write(k);
});

feed("ssh " + USER + "@" + HOST + ":" + PORT + " ...\n");
redraw();
ui.keyboard(1);
ssh.connect(HOST, PORT, USER, PASS, COLS, ROWS);
