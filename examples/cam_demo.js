// @app cam_demo
// @title カメラデモ
// @icon 
// @desc バーコードスキャナのデモ。スキャン中は画面上部にファインダーが出る。
//
// camera.* API の最小デモ:
//   camera.scan(fn[, prefix]) - 45 秒間スキャン。読めたら fn(コード)、
//                               だめなら fn(undefined)。prefix "97" で
//                               ISBN (978/979) だけに絞れる
//   camera.cancel()           - 実行中のスキャンを中断
//   camera.status()           - 直近の状態文字列 (デバッグ用)
// スキャン中はファインダー (ライブプレビュー) が自動表示される。
"use strict";
sys.setAppName("cam_demo");

var HAS_UI = ui.size()[0] !== 0;

var lastCode = "(まだ)";
var nCodes = 0;
var looping = false;
/* θファン累計 (連続モードでスキャン毎の camera.status() から集計)。
   fan<試行>/<救済> — 救済 = 角度リトライが同一フレームで拾った数 */
var fanRuns = 0, fanHits = 0;

function fanOf(st) {
    var m = /fan(\d+)\/(\d+)/.exec(st);
    return m ? [+m[1], +m[2]] : null;
}

function build() {
    var s = ui.screen("カメラデモ");
    if (typeof camera === "undefined") {
        s.label("このファームウェアに camera.* がありません");
        s.button("戻る", ui.back);
        return;
    }
    s.label("スキャン中は上部にファインダーが出ます");
    var status = s.label("状態: " + camera.status());
    var result = s.label("読んだコード: " + lastCode +
                         (nCodes ? " (計 " + nCodes + " 回)" : ""));
    var fan = s.label("θファン累計: 試行 " + fanRuns + " / 救済 " + fanHits);
    function tally(code) {
        var st = camera.status();
        var f = fanOf(st);
        if (f) {
            fanRuns += f[0];
            fanHits += f[1];
        }
        fan.setText("θファン累計: 試行 " + fanRuns + " / 救済 " + fanHits +
                    (f ? "  (今回 " + f[0] + "/" + f[1] + ")" : ""));
        if (code) {
            nCodes++;
            lastCode = code;
            result.setText("読んだコード: " + code + " (計 " + nCodes + " 回)");
        }
        return st;
    }
    function onCode(code) {
        var st = tally(code);
        status.setText(code ? "読み取り成功!" : "読めませんでした: " + st);
    }
    /* 連続モード: 読めても読めなくても即再スキャン。θファンの実証用
       (本を傾けて持ったまま、救済カウントが立つかを見る)。 */
    function loopScan(code) {
        var st = tally(code);
        if (!looping)
            return;
        if (camera.scan(loopScan, "97"))
            status.setText("連続スキャン中... " + st);
        else {
            looping = false;
            status.setText("再開できず: " + camera.status());
        }
    }
    s.button("連続スキャン 開始/停止 (θファン実証)", function () {
        if (looping) {
            looping = false;
            camera.cancel();
            status.setText("連続スキャン停止");
            return;
        }
        looping = true;
        if (camera.scan(loopScan, "97"))
            status.setText("連続スキャン中... 本を傾けてかざして");
        else {
            looping = false;
            status.setText("開始できず: " + camera.status());
        }
    });
    s.button("スキャン (ISBN だけ: 978/979)", function () {
        var ok = camera.scan(onCode, "97");
        status.setText(ok ? "スキャン中 (45 秒)... バーコードをかざして"
                          : "開始できず: " + camera.status());
    });
    s.button("スキャン (どの EAN-13 でも)", function () {
        var ok = camera.scan(onCode);
        status.setText(ok ? "スキャン中 (45 秒)... 食品の JAN なども OK"
                          : "開始できず: " + camera.status());
    });
    s.button("キャンセル", function () {
        looping = false;
        camera.cancel();
    });
    s.button("状態を表示", function () {
        status.setText("状態: " + camera.status());
    });
    s.button("コンソールへ戻る", ui.back);
}

/* 背面に回ったら連続モードは止める (カメラ+PPA は前面の道具) */
sys.onBackground(function () {
    if (looping) {
        looping = false;
        camera.cancel();
    }
});

if (HAS_UI) {
    build();
    sys.onForeground(build);
} else {
    print("cam_demo: 画面のある機体で使ってください");
    print("camera.status() = " +
          (typeof camera !== "undefined" ? camera.status() : "(no api)"));
}
