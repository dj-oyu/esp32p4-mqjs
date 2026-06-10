// I2C バススキャン + BMI270 チェック。Tab5 の内蔵バス向け設定
// (port 0, SDA=G31, SCL=G32, 400kHz)。他のボードはピンを合わせること。
// スクリプトは毎回走り切る → イベントループ終了 → 約 1 秒後に自動再実行。

i2c.setup(0, 31, 32, 400000);

var found = i2c.scan(0);
print("i2c scan:", found.length, "device(s)");
for (var i = 0; i < found.length; i++) {
    print("  0x" + found[i].toString(16));
}

// BMI270 (IMU) がいれば CHIP_ID (reg 0x00) を読む。0x24 が正解
for (var k = 0; k < found.length; k++) {
    var a = found[k];
    if (a == 0x68 || a == 0x69) {
        var id = i2c.readReg(0, a, 0x00, 1);
        print("BMI270@0x" + a.toString(16) + " chip_id = 0x" +
              id[0].toString(16) + (id[0] == 0x24 ? " (OK)" : " (?)"));
    }
}

delay(2000); // 次のスキャンまで少し待つ (再実行は app_main のループ任せ)
