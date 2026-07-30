// @app touch_demo
// @title お絵かき
// @icon 
// @desc キャンバスお絵かき。色と太さは設定ページで変更。
/* Tab5 タッチデモ: ハイブリッド UI のサンプル。
 *
 *  - 描画 = キャンバスモード (ui.line/rect)。タッチ座標を直接コマンド
 *    キューへ流す性能バイパスで、ウィジェットは経由しない。
 *  - 設定 (ペン色・太さ・クリア) = ウィジェットモード (ui.screen)。
 *    右上の青い □ をタップで開く。
 *
 * 標準イディオム (examples/README.md):
 *  - 設定画面が開いている間は inSettings フラグで onTouch を遮断
 *  - 閉じるのは必ず ui.back() (リテインスタックが画面を回収)
 * PC では onTouch / ウィジェットイベントは発火しない (スタブ)。 */
"use strict";
sys.setAppName("touch_demo");

var sz = ui.size();
var W = sz[0], H = sz[1];
if (!W)
    print("ui: この機体に画面はありません");

var BG = 0x101820;
var COLORS = [0x4FC3F7, 0x2ECC71, 0xFFD479, 0xE05A4E, 0xC678DD, 0xFFFFFF];
var NAMES = ["水色", "緑", "黄", "赤", "マゼンタ", "白"];
var ci = 0;
var brush = 2;            /* 線の太さ (px) */
var px = -1, py = -1;
var inSettings = false;

function scene() {
    ui.clear(BG);
    ui.text(16, 12, "タッチデモ: なぞって描く / 右上の □ で設定", 0xE0E6EA);
    ui.rect(W - 72, 8, 56, 56, 0x2E6BD6); /* 設定を開くボタン */
}

function openSettings() {
    inSettings = true;
    var s = ui.screen("お絵かき設定");
    var st = s.label("色: " + NAMES[ci] + " / 太さ: " + brush + "px");
    s.label("ペン色");
    var l = s.list();
    var k;
    for (k = 0; k < COLORS.length; k++) {
        (function (idx) {
            l.add(NAMES[idx], function () {
                ci = idx;
                st.setText("色: " + NAMES[ci] + " / 太さ: " + brush + "px");
            });
        })(k);
    }
    s.slider(1, 8, brush, function (v) {
        brush = v;
        st.setText("色: " + NAMES[ci] + " / 太さ: " + brush + "px");
    });
    s.button("キャンバスをクリア", function () {
        scene(); /* 隠れているキャンバスに描いておく (戻ると反映済み) */
    });
    s.button("閉じる", function () {
        inSettings = false;
        ui.back();
    });
}

/* 回転すると C 側がキャンバスを swap した寸法で貼り直して隠すので、描き直すまで画面は空。
 * W を取り直さないと設定ボタンもその当たり判定も 720 幅の位置に取り残される。
 * 描きかけのストロークは回転前の座標なので切る (次の move で古い点から線が伸びる)。 */
function relayout() {
    var s = ui.size();
    W = s[0] || W;
    H = s[1] || H;
    px = -1;
    py = -1;
    scene();
}

scene();

ui.onKey(function (k) {
    if (k.charCodeAt(0) === 0 && k.slice(1) === "rotate")
        relayout(); /* 設定画面が前面でも処理する (無視すると戻ったときズレたまま) */
});

/* トークンはフォアグラウンドのアプリにしか届かない。取りこぼした一回がズレっぱなしになる */
setInterval(function () {
    var s = ui.size();
    if (s[0] !== W || s[1] !== H)
        relayout();
}, 500);

/* 背面では描画コマンドが捨てられる — 上のポーリングだけだと、裏で回転を
 * 拾って W だけ新しくなり、前面に戻っても寸法が一致するので二度と描き直さない */
sys.onForeground(relayout);

ui.onTouch(function (x, y, kind) {
    if (inSettings)
        return; /* ウィジェット画面が前面: キャンバスは触らない */
    if (kind === 0) { /* down */
        if (x > W - 80 && y < 72) {
            openSettings();
            return;
        }
        ui.rect(x - brush, y - brush, brush * 2 + 1, brush * 2 + 1, COLORS[ci]);
        px = x;
        py = y;
    } else if (kind === 1 && px >= 0) { /* move */
        var t;
        for (t = 0; t < brush; t++) {
            ui.line(px + t, py, x + t, y, COLORS[ci]);
            ui.line(px, py + t, x, y + t, COLORS[ci]);
        }
        px = x;
        py = y;
    } else if (kind === 2) { /* up */
        px = -1;
    }
});
