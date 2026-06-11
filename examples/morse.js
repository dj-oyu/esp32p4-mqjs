/*
 * morse.js - blink a message in Morse code on the LED (G2), forever.
 *
 * The schedule is a flat [level, duration] timeline played back with a
 * chained setTimeout, so only one timer slot is used and the watchdog
 * is re-armed on every step.
 *
 * Timing note: the event loop runs on a 100 Hz FreeRTOS tick, so
 * durations are quantized to 10 ms. UNIT must stay well above that.
 *
 * 画面あり (Tab5) なら W1 ウィジェットでメッセージを差し替えられる:
 * FIELD に入力して「送信」(フィールドはタップでオンスクリーンキーボード)。
 * LED 再生コアはヘッドレスでも従来どおり (HAS_UI ゲート、README 参照)。
 */
"use strict";

var HAS_UI = ui.size()[0] !== 0;

var LED = 2;
var UNIT = 120;            /* ms per Morse unit */
var MESSAGE = 'HELLO P4';

var CHARS = 'ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789';
var CODES = [
    '.-',    '-...',  '-.-.',  '-..',   '.',     '..-.',
    '--.',   '....',  '..',    '.---',  '-.-',   '.-..',
    '--',    '-.',    '---',   '.--.',  '--.-',  '.-.',
    '...',   '-',     '..-',   '...-',  '.--',   '-..-',
    '-.--',  '--..',
    '-----', '.----', '..---', '...--', '....-',
    '.....', '-....', '--...', '---..', '----.'
];

function codeOf(ch) {
    var i = CHARS.indexOf(ch);
    return i < 0 ? null : CODES[i];
}

gpio.setMode(LED, gpio.OUT);

/* compile MESSAGE into a [[level, units], ...] timeline */
function compile(msg) {
    var seq = [];
    var text = msg.toUpperCase();
    for (var i = 0; i < text.length; i++) {
        var ch = text.charAt(i);
        if (ch === ' ') {
            seq.push([0, 7]);          /* word gap */
            continue;
        }
        var code = codeOf(ch);
        if (!code)
            continue;                  /* unsupported char: skip */
        for (var j = 0; j < code.length; j++) {
            seq.push([1, code.charAt(j) === '.' ? 1 : 3]);
            seq.push([0, 1]);          /* intra-letter gap */
        }
        seq[seq.length - 1] = [0, 3];  /* letter gap replaces last gap */
    }
    seq.push([0, 14]);                 /* pause before repeating */
    return seq;
}

function dotsOf(msg) {
    var dots = '';
    var upper = msg.toUpperCase();
    for (var i = 0; i < upper.length; i++) {
        var c = upper.charAt(i);
        dots += (c === ' ') ? ' / ' : (codeOf(c) || '?') + ' ';
    }
    return dots;
}

var seq = compile(MESSAGE);
print('sending forever:', MESSAGE, '=>', dotsOf(MESSAGE));

var pos = 0;
function step() {
    if (pos >= seq.length) {
        pos = 0;
        print('repeat', '(' + performance.now() + ' ms)');
    }
    var ev = seq[pos++];
    gpio.write(LED, ev[0]);
    setTimeout(step, ev[1] * UNIT);
}
step();

if (HAS_UI) {
    var s = ui.screen("モールス送信機 (LED G2)");
    var stNow = s.label("送信中: " + MESSAGE);
    var stDots = s.label(dotsOf(MESSAGE));
    var f = s.field("メッセージ (A-Z 0-9 スペース)");
    f.setText(MESSAGE);
    s.button("この内容で送信", function () {
        var m = f.value();
        if (!m.length)
            return;
        MESSAGE = m;
        seq = compile(MESSAGE);
        pos = 0;                       /* 次の step から新タイムライン */
        stNow.setText("送信中: " + MESSAGE);
        stDots.setText(dotsOf(MESSAGE));
        print('message changed:', MESSAGE, '=>', dotsOf(MESSAGE));
    });
    s.button("コンソールへ戻る", ui.back);
}
