// @app ui_console_test
// @title コンソール試験
// @icon 
// @desc ANSI 色・長行分割・タブ展開のコンソール表示試験。
/* Tab5 コンソール表示試験: 日本語/長行 (96B 分割)、タブ展開、
 * ANSI カラー (SGR -> LVGL recolor 変換)、'#' エスケープ、
 * tail-follow スクロール。 */
"use strict";
sys.setAppName("ui_console_test");
var ESC = "\x1b[";
var n = 0;
print(ESC + "33m== コンソール表示テスト ==" + ESC + "0m");
print("タブ:\tA\tB\tC (4 スペースに展開)");
print("記号: # ## %% & @ ! ? ()[]{} ハイフン- スラッシュ/");
print(ESC + "31m赤 " + ESC + "32m緑 " + ESC + "33m黄 " + ESC + "34m青 " +
      ESC + "35mマゼンタ " + ESC + "36mシアン" + ESC + "0m 標準");
print(ESC + "91m明るい赤 " + ESC + "92m明るい緑 " + ESC + "96m明るいシアン" +
      ESC + "0m おわり");
print("色持ち越し: " + ESC + "32mこの行の途中から緑になって…");
print("…次の行もまだ緑のまま。" + ESC + "0mここで標準に戻る");
setInterval(function () {
    n++;
    print("行 " + n + ": こんにちは世界 / mqjs コンソール試験");
    if (n % 5 === 0)
        print(ESC + "36m長い行テスト: " +
              "あいうえおかきくけこさしすせそたちつてとなにぬねの" +
              "abcdefghijklmnopqrstuvwxyz0123456789" + ESC + "0m");
}, 1000);
