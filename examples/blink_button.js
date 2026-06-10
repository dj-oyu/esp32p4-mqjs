/*
 * blink_button.js - the original first-milestone demo.
 *
 * Wiring: LED + resistor on G2 to GND, button on G5 to GND.
 * Blinks the LED at 1 Hz and reports button edges on the console.
 */
print('JS task started');
var LED = 2, BTN = 5;
gpio.setMode(LED, gpio.OUT);
gpio.setMode(BTN, gpio.IN_PULLUP);

var on = false;
setInterval(function() {
    on = !on;
    gpio.write(LED, on ? 1 : 0);
}, 500);

gpio.onChange(BTN, function(level) {
    print('button level:', level, 'at', performance.now(), 'ms');
});
