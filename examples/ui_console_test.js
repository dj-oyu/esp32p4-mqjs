/* Phase 1 acceptance test: stream mixed JP/ASCII lines to the Tab5
 * console. Long lines exercise the 96-byte UTF-8-safe split, the loop
 * exercises tail-follow scrolling. */
"use strict";
var n = 0;
setInterval(function () {
    n++;
    print("行 " + n + ": こんにちは世界 / mqjs コンソール試験");
    if (n % 5 === 0)
        print("長い行テスト: " +
              "あいうえおかきくけこさしすせそたちつてとなにぬねの" +
              "abcdefghijklmnopqrstuvwxyz0123456789");
}, 1000);
