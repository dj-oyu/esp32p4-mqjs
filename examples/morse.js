/*
 * morse.js - blink a message in Morse code on the LED (G2), forever.
 *
 * The schedule is a flat [level, duration] timeline played back with a
 * chained setTimeout, so only one timer slot is used and the watchdog
 * is re-armed on every step.
 *
 * Timing note: the event loop runs on a 100 Hz FreeRTOS tick, so
 * durations are quantized to 10 ms. UNIT must stay well above that.
 */
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

var seq = compile(MESSAGE);

/* pretty-print what we are about to send */
var dots = '';
var upper = MESSAGE.toUpperCase();
for (var i = 0; i < upper.length; i++) {
    var c = upper.charAt(i);
    dots += (c === ' ') ? ' / ' : (codeOf(c) || '?') + ' ';
}
print('sending forever:', MESSAGE, '=>', dots);

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
