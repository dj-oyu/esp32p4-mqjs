/*
 * blink_button.js - the original first-milestone demo.
 *
 * Wiring: LED + resistor on G2 to GND, button on G5 to GND.
 * Blinks the LED at 1 Hz and reports button edges on the console.
 *
 * 画面あり (Tab5) なら W1 ウィジェットのコントロールパネルを重ねる:
 * 点滅トグル / 周期スライダー / ボタンエッジの表示 (setText)。
 * GPIO コアはヘッドレスでも従来どおり (HAS_UI ゲート、README 参照)。
 */
"use strict";

var HAS_UI = ui.size()[0] !== 0;

print('JS task started');
var LED = 2, BTN = 5;
gpio.setMode(LED, gpio.OUT);
gpio.setMode(BTN, gpio.IN_PULLUP);

var on = false;
var periodMs = 500;
var blinkTimer = -1;

function startBlink() {
    if (blinkTimer >= 0)
        clearInterval(blinkTimer);
    blinkTimer = setInterval(function () {
        on = !on;
        gpio.write(LED, on ? 1 : 0);
    }, periodMs);
}

function stopBlink() {
    if (blinkTimer >= 0) {
        clearInterval(blinkTimer);
        blinkTimer = -1;
    }
    gpio.write(LED, 0);
}

startBlink();

var stBtn = null;
gpio.onChange(BTN, function (level) {
    print('button level:', level, 'at', performance.now(), 'ms');
    if (stBtn)
        stBtn.setText("ボタン: level=" + level + " @" +
                      (performance.now() | 0) + "ms");
});

if (HAS_UI) {
    var s = ui.screen("Lチカ コントロール (G2/G5)");
    s.toggle("点滅", 1, function (v) {
        if (v) startBlink(); else stopBlink();
    });
    s.label("周期 (ms)");
    s.slider(100, 2000, periodMs, function (v) {
        periodMs = v;
        if (blinkTimer >= 0)
            startBlink(); /* 新しい周期で張り直し */
    });
    stBtn = s.label("ボタン: (まだ)");
    s.button("コンソールへ戻る", ui.back);
}
