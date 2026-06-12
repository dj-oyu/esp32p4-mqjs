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

function build() {
    var s = ui.screen("カメラデモ");
    if (typeof camera === "undefined") {
        s.label("このファームウェアに camera.* がありません");
        s.button("戻る", ui.back);
        return;
    }
    s.label("スキャン中は上部にファインダーが出ます");
    var status = s.label("状態: " + camera.status());
    var result = s.label("最後に読んだコード: " + lastCode);
    function onCode(code) {
        if (code) {
            lastCode = code;
            result.setText("最後に読んだコード: " + code);
            status.setText("読み取り成功!");
            sys.notify("バーコード: " + code);
        } else {
            status.setText("読めませんでした: " + camera.status());
        }
    }
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
        camera.cancel();
    });
    s.button("状態を表示", function () {
        status.setText("状態: " + camera.status());
    });
    s.button("コンソールへ戻る", ui.back);
}

if (HAS_UI) {
    build();
    sys.onForeground(build);
} else {
    print("cam_demo: 画面のある機体で使ってください");
    print("camera.status() = " +
          (typeof camera !== "undefined" ? camera.status() : "(no api)"));
}
