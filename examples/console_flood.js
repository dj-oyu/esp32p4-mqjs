// console_flood.js — W1-1 regression: the old 64KB builtin pool froze the
// LVGL task at ~200 console labels (lv_malloc NULL while holding the port
// lock). Flood well past the 200-line ring, then call sys.heap() — that
// takes the LVGL port lock, so if the UI task died this script hangs and
// never prints the OK line. Seeing "console regression OK" = UI alive.
"use strict";
sys.setAppName("console_flood");
var h0 = sys.heap();
print("flood start: lvgl=" + h0[2]);
var n = 0;
var t = setInterval(function () {
  for (var i = 0; i < 10; i++) {
    n++;
    print("\x1b[3" + (n % 8) + "m行 " + n +
          ": こんにちは世界 console flood あいうえお ABCDEFG 0123456789\x1b[0m");
  }
  if (n >= 300) {
    clearInterval(t);
    var h1 = sys.heap(); // blocks forever if the LVGL task is dead
    print("console regression OK: 300 lines, lvgl " + h0[2] + " -> " + h1[2] +
          " (d=" + (h1[2] - h0[2]) + "), int d=" + (h1[0] - h0[0]));
  }
}, 200);
