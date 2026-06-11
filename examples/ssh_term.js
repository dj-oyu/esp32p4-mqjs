/* Tab5 SSH ターミナル (Phase T1): オンスクリーンキーボード + ssh.* で
 * 本物の SSH シェルに繋ぐ最小ターミナル。サーバの出力をコンソール風に
 * 流し、キーボード入力をそのまま送る。VT エスケープの厳密な解釈はせず、
 * 印字可能文字と改行・BS だけ扱う (本格版は ssh_vt.js)。
 *
 * 接続先はハードコードせず、W1 ウィジェットの接続フォームで入力する
 * (ハイブリッド UI: フォーム = ウィジェット、端末描画 = キャンバス)。
 * 認証情報がスクリプトに残らないので、このファイルはそのまま共有できる。
 * 切断されるとフォームに戻る。PC ではウィジェットイベントが発火しない
 * ためフォームから先へは進まない (端末コアの構文チェックのみ)。 */
"use strict";

var sz = ui.size();
var W = sz[0], H = sz[1];
if (!W)
    print("ui: この機体に画面はありません (PC スタブ)");

var BG = 0x0B0E11;
var FG = 0xC9D1D9;
var cell = ui.textSize("M");      /* 等幅近似: 半角 1 マス */
var CW = cell[0] || 10, LH = (cell[1] || 25) + 2;
var KB_H = 400;
var VIEW_H = H - KB_H;
var COLS = 80;
var ROWS = 24;

/* ---- 端末コア (キャンバスモード: ui.rect/text 直描き) ---- */

var lines = [""];
var dirty = true;
var running = false;     /* 端末 / フォームのどちらが前面か */

function putChar(c) {
    var code = c.charCodeAt(0);
    if (c === "\n") {
        lines.push("");
    } else if (c === "\r") {
        /* キャリッジリターン: 上書きは簡略化で無視 */
    } else if (code === 8 || code === 127) { /* BS / DEL */
        var ln = lines[lines.length - 1];
        if (ln.length)
            lines[lines.length - 1] = ln.slice(0, -1);
    } else if (code === 27) {
        /* ESC シーケンス: この最小版では無視 */
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
    if (!dirty || !running)
        return;
    dirty = false;
    ui.rect(0, 0, W, VIEW_H, BG);
    var start = lines.length > ROWS ? lines.length - ROWS : 0;
    for (var i = start; i < lines.length; i++)
        ui.text(2, (i - start) * LH + 2, lines[i], FG);
}

/* W3: ssh.* はハンドル式 — connect が返す id を全呼び出しに渡す */
var sid = 0;

ui.onKey(function (k) {
    if (!sid)
        return;
    /* キーはそのままサーバへ。Enter は CR (端末の慣習) で送る */
    if (k === "\n")
        ssh.write(sid, "\r");
    else if (k === "\b")
        ssh.write(sid, "\x7f"); /* DEL: bash の行編集が期待する */
    else
        ssh.write(sid, k);
});

function startTerm(host, port, user, pass) {
    running = true;
    lines = [""];
    feed("ssh " + user + "@" + host + ":" + port + " ...\n");
    dirty = true;
    redraw();
    ui.keyboard(1);
    sid = ssh.connect(host, port, user, pass, COLS, ROWS);
    ssh.onData(sid, function (chunk) {
        feed(chunk);
        redraw();
    });
    ssh.onClose(sid, function (reason) {
        sid = 0;
        feed("\n*** SSH closed: " + reason + " ***\n");
        redraw();
        ui.keyboard(0);
        running = false;
        connectForm("切断: " + reason); /* フォームに戻る (再接続 UX) */
    });
}

/* ---- 接続フォーム (ウィジェットモード) ---- */

function connectForm(note) {
    var s = ui.screen("SSH 接続 (T1 ミニ端末)");
    if (note)
        s.label(note);
    var host = s.field("Host");
    host.setText("192.168.1.10");
    var port = s.field("Port");
    port.setText("22");
    var user = s.field("User");
    user.setText("pi");
    var pass = s.field("Password", { secret: true });
    s.button("接続", function () {
        var p = parseInt(port.value(), 10);
        ui.back(); /* キャンバス (コンソール) 画面に戻ってから端末開始 */
        startTerm(host.value(), isNaN(p) ? 22 : p, user.value(), pass.value());
    });
}

connectForm(null);
