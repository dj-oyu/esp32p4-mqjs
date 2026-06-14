// Firmware-embedded device settings system app.
// Not installed, updated, or replaced through the MQTT app store.
"use strict";

sys.setAppName("device_settings");

function unwind() {
    while (ui.back()) {}
}

function mainPage() {
    unwind();
    var s = ui.screen("デバイス設定");
    s.label("ネットワーク");
    var list = s.list();
    var wifi = system.wifiStatus();
    var ts = system.tailscaleStatus();
    list.add("Wi-Fi    " + (wifi.configured ? wifi.ssid : "未設定"),
             wifiPage);
    list.add("Tailscale    " + (ts.configured ? "設定済み" : "未設定"),
             tailscalePage);
    list.add("QR読み取りテスト", qrTestPage);
    s.label("この画面は端末ファームウェアに組み込まれています。");
    s.label("現在は保存モックです。接続処理はまだ変更しません。");
    s.button("アプリ一覧へ戻る", function () {
        sys.open("launcher");
    });
}

function qrTestPage() {
    var s = ui.screen("QR読み取りテスト");
    var result = s.label("QRコードの内容を一時表示します。");
    s.label("結果は保存・ログ出力されません。秘密値の表示に注意してください。");
    s.button("QRコードを読み取る", function () {
        result.setText("読み取り中...");
        if (!camera.scanQr(function (text) {
            result.setText(text === undefined
                ? "読み取れませんでした\n" + camera.status()
                : "読み取り結果:\n" + text + "\n\n性能:\n" + camera.status());
        }))
            result.setText("カメラを開始できませんでした");
    });
    s.button("キャンセル", function () {
        camera.cancel();
    });
    s.button("戻る", mainPage);
}

function wifiPage() {
    var status = system.wifiStatus();
    var s = ui.screen("Wi-Fi");
    var message = s.label(status.configured
        ? "保存済み: " + status.ssid
        : "保存済み設定はありません");
    var ssid = s.field("SSID");
    var password = s.field("パスワード", { secret: true });
    s.button("保存", function () {
        if (!ssid.value()) {
            message.setText("SSIDを入力してください");
            return;
        }
        if (system.wifiSet(ssid.value(), password.value())) {
            message.setText("保存しました。接続処理は未実装です");
            password.setText("");
        } else {
            message.setText("保存できませんでした");
        }
    });
    if (status.configured) {
        s.button("保存済み設定を削除", function () {
            message.setText(system.wifiForget()
                ? "削除しました"
                : "削除できませんでした");
        });
    }
    s.button("戻る", mainPage);
}

function tailscalePage() {
    var status = system.tailscaleStatus();
    var s = ui.screen("Tailscale");
    var message = s.label(status.configured
        ? "Auth keyは設定済みです"
        : "Auth keyは未設定です");
    var authKey = s.field("Auth key", { secret: true });
    s.button("保存", function () {
        if (!authKey.value()) {
            message.setText("Auth keyを入力してください");
            return;
        }
        if (system.tailscaleSet(authKey.value())) {
            message.setText("保存しました。microlink接続は未実装です");
            authKey.setText("");
        } else {
            message.setText("保存できませんでした");
        }
    });
    if (status.configured) {
        s.button("保存済みAuth keyを削除", function () {
            message.setText(system.tailscaleForget()
                ? "削除しました"
                : "削除できませんでした");
        });
    }
    s.button("戻る", mainPage);
}

sys.onForeground(mainPage);
mainPage();
