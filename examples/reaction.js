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
 */
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

print('=== reaction game ===');
print('press the button to start a round');

function arm() {
    state = 'armed';
    gpio.write(LED, 0);
    var wait = 1000 + (Math.random() * 2000 | 0);
    print('wait for the LED ...');
    goTimer = setTimeout(function() {
        state = 'go';
        t0 = performance.now();
        gpio.write(LED, 1);
    }, wait);
}

gpio.onChange(BTN, function(level) {
    var now = performance.now();
    if (now - lastEdge < DEBOUNCE_MS)
        return;                       /* contact bounce */
    lastEdge = now;
    if (level !== 0)
        return;                       /* react on press only */

    if (state === 'idle') {
        arm();
    } else if (state === 'armed') {
        clearTimeout(goTimer);
        state = 'idle';
        print('false start! press to try again');
    } else if (state === 'go') {
        var dt = now - t0;
        gpio.write(LED, 0);
        state = 'idle';
        if (best < 0 || dt < best) {
            best = dt;
            print('reaction: ' + dt + ' ms  *** new best ***');
        } else {
            print('reaction: ' + dt + ' ms  (best ' + best + ' ms)');
        }
        print('press to play again');
    }
});
