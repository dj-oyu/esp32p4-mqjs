// @app audio_test
// @title 音テスト
// @icon
// @desc ES8388 スピーカーのテスト。ビープ音と再生テレメトリを確認。
// @perm audio
// audio.* の最小デモ / P2 ゲート検証 (opus-decoder-plan)。
// デコード済み PCM は C 側 (Opus パス) から audio_tab5_write へ直接
// 流れるので、JS 側はスカラ制御 + テレメトリのみ:
//   audio.start(rate, ch) -> bool   再生パス起動 (既定 48000, 2)
//   audio.stop()                    リングをドレインして停止
//   audio.tone(hz, ms)    -> bool   非ブロッキングのビープ (一度に1音)
//   audio.volume([pct])   -> 0..100 引数ありで設定
//   audio.stats()         -> JSON   running/rate/ch/queued/underruns/frames
//
// 画面なし機ではビープ列を鳴らして stats を print。Tab5 ならボタン付き
// パネルを出す。PC (run_pc) ではすべてスタブ (false / "off") で安全。
"use strict";
sys.setAppName("audio_test");

var HAS_UI = ui.size()[0] !== 0;

function beep(hz, ms) {
    var ok = audio.tone(hz, ms);
    print("[audio] tone(" + hz + "," + ms + ") -> " + ok);
    return ok;
}

if (HAS_UI) {
    var s = ui.screen("音テスト");
    if (typeof audio === "undefined") {
        s.label("このファームウェアに audio.* がありません");
        s.button("戻る", ui.back);
    } else {
        audio.start(48000, 2);
        var vol = audio.volume();
        var stat = s.label("stats: " + audio.stats());
        var volLbl = s.label("音量: " + vol + "%");
        var refresh = function () { stat.setText("stats: " + audio.stats()); };
        s.button("ビープ 440Hz", function () { beep(440, 300); setTimeout(refresh, 500); });
        s.button("ビープ 880Hz", function () { beep(880, 300); setTimeout(refresh, 500); });
        s.button("和音 (上昇)", function () {
            beep(523, 200);
            setTimeout(function () { beep(659, 200); }, 250);
            setTimeout(function () { beep(784, 250); }, 500);
            setTimeout(refresh, 900);
        });
        s.button("音量 +10", function () {
            vol = audio.volume(Math.min(100, vol + 10));
            volLbl.setText("音量: " + vol + "%");
        });
        s.button("音量 -10", function () {
            vol = audio.volume(Math.max(0, vol - 10));
            volLbl.setText("音量: " + vol + "%");
        });
        s.button("stats 更新", refresh);
        s.button("停止", function () { audio.stop(); refresh(); });
        s.button("戻る", ui.back);
    }
} else {
    // ヘッドレス: ビープ列を鳴らし、stats を定期 print。MQTT があれば
    // テレメトリをトピックに publish (実機の遠隔 P2 検証用 — COM 不要)。
    var TOPIC = "esp32p4-mqjs/audio/stats";
    audio.start(48000, 2);
    audio.volume(70);
    print("[audio_test] start: " + audio.stats());

    var seq = [[440, 250], [554, 250], [659, 300]];
    var i = 0;
    var report = function () {
        var st = audio.stats();
        print("[audio_test] " + st);
        if (typeof mqtt !== "undefined" && mqtt.connected && mqtt.connected())
            mqtt.publish(TOPIC, st);
    };
    var playNext = function () {
        if (i >= seq.length) {
            report();
            return;
        }
        beep(seq[i][0], seq[i][1]);
        i++;
        setTimeout(playNext, 400);
    };
    setTimeout(playNext, 300);
    setInterval(report, 5000);
}
