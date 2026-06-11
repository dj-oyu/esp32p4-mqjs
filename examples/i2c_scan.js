// I2C バススキャン + BMI270 チェック。ウィジェットモードのサンプル。
// Tab5 の内蔵バス向け設定 (port 0, SDA=G31, SCL=G32, 400kHz)。
// 他のボードはピンを合わせること。
//
// 画面あり (Tab5): 結果を ui.screen のリストに表示し、「再スキャン」で
//   画面ごと作り直す (動的リストの標準パターン: 行の個別削除 API は無く、
//   リテインスタック N=3 が古い画面を回収する)。
// 画面なし (Stamp/PC): HAS_UI ゲートでウィジェットを作らないので、従来
//   どおり print → イベントループ終了 → 約 3 秒周期で自動再実行が保たれる。
"use strict";

var HAS_UI = ui.size()[0] !== 0;

i2c.setup(0, 31, 32, 400000);

function hex(a) { return "0x" + a.toString(16); }

/* スキャンして [{addr, note}] を返す。コンソールにも従来どおり print。 */
function scan() {
    var found = i2c.scan(0);
    print("i2c scan:", found.length, "device(s)");
    var out = [];
    for (var i = 0; i < found.length; i++) {
        var a = found[i];
        var note = "";
        if (a === 0x68 || a === 0x69) {
            /* BMI270 (IMU) なら CHIP_ID (reg 0x00) を読む。0x24 が正解 */
            var id = i2c.readReg(0, a, 0x00, 1);
            note = " BMI270 chip_id=" + hex(id[0]) +
                   (id[0] === 0x24 ? " (OK)" : " (?)");
        }
        print("  " + hex(a) + note);
        out.push({ addr: a, note: note });
    }
    return out;
}

function scanScreen() {
    var found = scan();
    var s = ui.screen("I2C スキャン (port0 G31/G32)");
    s.label(found.length + " 台見つかりました");
    var st = s.label("(行をタップすると reg0 を読みます)");
    var l = s.list();
    for (var i = 0; i < found.length; i++) {
        (function (d) {
            l.add(hex(d.addr) + d.note, function () {
                /* 行タップ: そのアドレスの reg0 を読んでみる */
                var v = i2c.readReg(0, d.addr, 0x00, 1);
                st.setText(hex(d.addr) + " reg0 = " + hex(v[0]));
            });
        })(found[i]);
    }
    s.button("再スキャン", function () { scanScreen(); });
    s.button("戻る", ui.back);
}

if (HAS_UI) {
    scanScreen(); /* ボタン操作で再スキャン (ループはウィジェットが保持) */
} else {
    scan();
    delay(2000); /* 次のスキャンまで少し待つ (再実行は app_main のループ任せ) */
}
