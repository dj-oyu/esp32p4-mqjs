// @app reaction
// @title 反射神経ゲーム
// @icon 
// @desc LED 点灯からボタン押下までの反応時間を計測。
// @perm gpio
/*
 * reaction.js - reaction-time game with one button and one LED.
 *
 * Wiring: LED + resistor on G2 to GND, button on G5 to GND (pull-up,
 * so pressed = 0).
 *
 * Press the button to arm. After a random 1-3 s pause the LED lights:
 * press again as fast as you can. Pressing during the pause is a
 * false start. Best time is tracked across rounds.
 *
 * Shows: gpio.onChange + JS-side debounce, clearTimeout, a small state
 * machine, performance.now timing.
 *
 * 画面あり (Tab5) なら W1 ウィジェットで遊べる: 物理ボタンの代わりに
 * 画面のボタンが同じ press() を叩き、状態と記録はラベルに setText。
 * (LED は配線があれば光る。無くてもゲームは成立する)
 */
"use strict";
sys.setAppName("reaction");

var HAS_UI = ui.size()[0] !== 0;

var LED = 2, BTN = 5;
var DEBOUNCE_MS = 40;

gpio.setMode(LED, gpio.OUT);
gpio.setMode(BTN, gpio.IN_PULLUP);
gpio.write(LED, 0);

var state = 'idle';        /* idle -> armed -> go -> idle */
var goTimer = -1;
var t0 = 0;
var best = -1;
var lastEdge = 0;

var stState = null, stBest = null;

function show(msg) {
    print(msg);
    if (stState)
        stState.setText(msg);
}

print('=== reaction game ===');
show('press the button to start a round');

function arm() {
    state = 'armed';
    gpio.write(LED, 0);
    var wait = 1000 + (Math.random() * 2000 | 0);
    show('wait for the GO sign ...');
    goTimer = setTimeout(function() {
        state = 'go';
        t0 = performance.now();
        gpio.write(LED, 1);
        show('*** GO! press now ***');
    }, wait);
}

/* 物理ボタンとウィジェットボタンの共通入口 */
function press() {
    if (state === 'idle') {
        arm();
    } else if (state === 'armed') {
        clearTimeout(goTimer);
        state = 'idle';
        show('false start! press to try again');
    } else if (state === 'go') {
        var dt = performance.now() - t0;
        gpio.write(LED, 0);
        state = 'idle';
        if (best < 0 || dt < best) {
            best = dt;
            show('reaction: ' + dt + ' ms  *** new best ***');
        } else {
            show('reaction: ' + dt + ' ms  (best ' + best + ' ms)');
        }
        if (stBest)
            stBest.setText('best: ' + best + ' ms');
        print('press to play again');
    }
}

gpio.onChange(BTN, function(level) {
    var now = performance.now();
    if (now - lastEdge < DEBOUNCE_MS)
        return;                       /* contact bounce */
    lastEdge = now;
    if (level !== 0)
        return;                       /* react on press only */
    press();
});

if (HAS_UI) {
    var s = ui.screen("反射神経ゲーム");
    stState = s.label("press to start");
    stBest = s.label("best: -");
    s.button("PRESS", press);
    s.button("コンソールへ戻る", ui.back);
}
