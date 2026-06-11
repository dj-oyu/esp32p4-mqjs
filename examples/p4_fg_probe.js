// P4a 実機検証プローブ 2: 前面切替ライフサイクルを自動で回す。
// 8 秒ごとに dev <-> bg_app の前面を入れ替える (ステータスバータップと
// 同じ switch_foreground 経路)。シリアル/コンソールで
//   "mqjs: foreground -> ..." / PROBE FOREGROUND / PROBE BACKGROUND /
//   [bg_app] background (tick=...)
// が周回し、bg_app の画面が復帰のたびに再構築されることを確認する。
"use strict";

sys.onForeground(function () { print("PROBE FOREGROUND"); });
sys.onBackground(function () { print("PROBE BACKGROUND"); });

var flip = false;
setInterval(function () {
    flip = !flip;
    sys.focus(flip ? 2 : 1);
}, 8000);

print("fg probe started");
