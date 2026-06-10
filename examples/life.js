/*
 * life.js - Conway's Game of Life animated on the serial console.
 *
 * No wiring needed: just `idf.py -DMQJS_SCRIPT=life.js build flash`
 * and watch the monitor (a terminal that understands ANSI escapes).
 *
 * Implementation notes for the mquickjs subset:
 *  - arrays cannot have holes, so grids are pre-sized with new Array(n)
 *    and every cell written before use
 *  - one frame per setInterval tick keeps each JS run far below the
 *    5 s watchdog
 */
var W = 48, H = 20;
var ESC = String.fromCharCode(27);
var ALIVE = '#', DEAD = ' ';

var grid = new Array(W * H);
var next = new Array(W * H);
var gen = 0;

function seed() {
    for (var i = 0; i < W * H; i++)
        grid[i] = (Math.random() < 0.25) ? 1 : 0;
    /* stamp a glider so there is always something moving */
    var g = [[1, 0], [2, 1], [0, 2], [1, 2], [2, 2]];
    for (var k = 0; k < g.length; k++)
        grid[(g[k][1] + 1) * W + g[k][0] + 1] = 1;
    gen = 0;
}

function stepWorld() {
    var pop = 0;
    for (var y = 0; y < H; y++) {
        var ym = ((y - 1 + H) % H) * W, y0 = y * W, yp = ((y + 1) % H) * W;
        for (var x = 0; x < W; x++) {
            var xm = (x - 1 + W) % W, xp = (x + 1) % W;
            var n = grid[ym + xm] + grid[ym + x] + grid[ym + xp] +
                    grid[y0 + xm] +                grid[y0 + xp] +
                    grid[yp + xm] + grid[yp + x] + grid[yp + xp];
            var alive = grid[y0 + x] ? (n === 2 || n === 3) : (n === 3);
            next[y0 + x] = alive ? 1 : 0;
            pop += next[y0 + x];
        }
    }
    var t = grid; grid = next; next = t;
    gen++;
    return pop;
}

function draw(pop) {
    var s = ESC + '[H';                       /* cursor home */
    s += '+' + '-'.repeat(W) + '+\n';
    for (var y = 0; y < H; y++) {
        var line = '|';
        for (var x = 0; x < W; x++)
            line += grid[y * W + x] ? ALIVE : DEAD;
        s += line + '|\n';
    }
    s += '+' + '-'.repeat(W) + '+\n';
    s += 'gen ' + gen + '  pop ' + pop + '   ';
    print(s);
}

print(ESC + '[2J');                           /* clear screen once */
seed();
setInterval(function() {
    var pop = stepWorld();
    draw(pop);
    if (pop === 0 || gen >= 500) {            /* died out or stale: reseed */
        seed();
    }
}, 150);
