sys.setAppName("mandelbrot");
/*
 * mandelbrot.js - ASCII Mandelbrot zoom on the serial console.
 *
 * Zooms into the "seahorse valley" one frame per timer tick (so the
 * 5 s watchdog is re-armed between frames), then lets the event loop
 * exit naturally — the host restarts the task and the zoom replays.
 *
 * The P4 has a double-precision FPU at 360 MHz: a 78x24 frame with
 * up to ~200 iterations per pixel renders in a few milliseconds.
 */
var W = 78, H = 24;
var CX = -0.743643, CY = 0.131825;   /* zoom target */
var FRAMES = 40;
var PALETTE = ' .:-=+*#%@';
var ESC = String.fromCharCode(27);

function renderFrame(f) {
    var scale = 3.0 * Math.pow(0.8, f);      /* width of the view */
    var maxIter = 40 + f * 4;
    var s = ESC + '[H';
    for (var py = 0; py < H; py++) {
        var line = '';
        var y0 = CY + (py / (H - 1) - 0.5) * scale * (H * 2.0 / W);
        for (var px = 0; px < W; px++) {
            var x0 = CX + (px / (W - 1) - 0.5) * scale;
            var x = 0, y = 0, it = 0;
            while (x * x + y * y <= 4 && it < maxIter) {
                var xt = x * x - y * y + x0;
                y = 2 * x * y + y0;
                x = xt;
                it++;
            }
            if (it >= maxIter) {
                line += ' ';
            } else {
                var idx = (it * (PALETTE.length - 1) / maxIter) | 0;
                line += PALETTE.charAt(PALETTE.length - 1 - idx);
            }
        }
        s += line + '\n';
    }
    return s;
}

print(ESC + '[2J');
var f = 0;
function step() {
    var t0 = performance.now();
    var s = renderFrame(f);
    var dt = performance.now() - t0;
    print(s + 'frame ' + (f + 1) + '/' + FRAMES +
          '  zoom ' + Math.pow(1 / 0.8, f).toFixed(1) + 'x' +
          '  rendered in ' + dt + ' ms   ');
    f++;
    if (f < FRAMES)
        setTimeout(step, 250);
    else
        print('zoom done - event loop will now exit, task restarts in 1s');
}
step();
