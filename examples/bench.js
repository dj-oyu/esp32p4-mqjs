/*
 * bench.js - micro-benchmarks for mquickjs on the ESP32-P4 (360 MHz).
 *
 * One benchmark per timer tick so every run gets a fresh 5 s watchdog
 * deadline. When the queue is empty the event loop exits and the host
 * restarts the task, so the suite repeats every ~1 s.
 *
 * Numbers to expect: the VM is a bytecode interpreter, but the P4 has
 * a hardware double FPU, so float code is comparatively fast.
 */
function pad(s, n) {
    s = '' + s;
    while (s.length < n)
        s += ' ';
    return s;
}

var benches = [
    ['fib(24) recursion', function() {
        function fib(n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
        return fib(24);                       /* 46368 */
    }],
    ['sieve 50k (Uint8Array)', function() {
        var N = 50000;
        var sieve = new Uint8Array(N);        /* zero-filled */
        var count = 0;
        for (var i = 2; i < N; i++) {
            if (!sieve[i]) {
                count++;
                for (var j = i + i; j < N; j += i)
                    sieve[j] = 1;
            }
        }
        return count;                         /* 5133 primes */
    }],
    /* string += copies the whole string every time (O(n^2)) and the
       context lives in PSRAM: keep this small or it blows the 5 s
       watchdog on the device */
    ['string build 20k chars', function() {
        var s = '';
        for (var i = 0; i < 2000; i++)
            s += '0123456789';
        return s.length;
    }],
    /* keep the array small: map+filter allocate full copies and the
       whole context is 256 KB */
    ['array map+filter+reduce 5k', function() {
        var a = new Array(5000);
        for (var i = 0; i < a.length; i++)
            a[i] = i;
        return a.map(function(x) { return x * 3; })
                .filter(function(x) { return x % 2 === 0; })
                .reduce(function(acc, x) { return acc + x; }, 0);
    }],
    /* sin/cos come from mquickjs's portable libm.c, not the FPU:
       ~30 us per call on the P4. 50k keeps us under the 5 s watchdog */
    ['Math.sin/cos x 50k', function() {
        var acc = 0;
        for (var i = 0; i < 50000; i++)
            acc += Math.sin(i * 0.001) * Math.cos(i * 0.002);
        return acc.toFixed(3);
    }],
    ['JSON round-trip x 200', function() {
        var obj = { name: 'stamp-p4', freq: 360, tags: ['riscv', 'psram'],
                    nested: { a: [1, 2, 3], b: true, c: null } };
        var out = 0;
        for (var i = 0; i < 200; i++)
            out = JSON.parse(JSON.stringify(obj)).freq;
        return out;
    }],
    ['regexp scan x 2k', function() {
        var re = /([a-z]+)-(\d+)/;
        var hits = 0;
        for (var i = 0; i < 2000; i++) {
            if (re.test('unit-' + i + ' of stamp-p4'))
                hits++;
        }
        return hits;
    }],
];

print('=== mquickjs micro-bench on ESP32-P4 @ 360 MHz ===');

var idx = 0;
var total = 0;
function runNext() {
    if (idx >= benches.length) {
        print(pad('TOTAL', 28) + pad(total + ' ms', 10));
        print('suite done - task will restart and run again in ~1s');
        return;                               /* no timers left: loop exits */
    }
    var name = benches[idx][0], fn = benches[idx][1];
    var t0 = performance.now();
    var result = fn();
    var dt = performance.now() - t0;
    total += dt;
    print(pad(name, 28) + pad(dt + ' ms', 10) + ' => ' + result);
    idx++;
    setTimeout(runNext, 10);
}
runNext();
