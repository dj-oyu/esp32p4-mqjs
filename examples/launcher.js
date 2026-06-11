// ランチャー (P4b): slot 0 常駐の組み込みアプリ。
//
// - アプリ一覧: 実行中 (●) はタップで即切替・行末の ✕ で即停止、
//   停止中 (○) はタップで起動 (= 開く)。dev タスクの再開行もここ。
//   確認ページは置かない: 停止の被害は「消える」だけで、○ 行 /
//   ステータスバーのチップから 1 タップで復活できる。
// - 「開く」要求の解決役: ステータスバーのチップ/通知タップは
//   {"op":"open","app":<name>} のシグナルとしてここに届く。動いていれば
//   focus、止まっていれば launch → focus (差出人は問わない: アプリからの
//   sys.signal("launcher", ...) でも同じに動く)。
// - C 側の不変条件により slot 0 は停止不可・死んでも自動再起動される。
//
// 画面は他アプリ同様「切替時に破棄、onForeground で再構築」。
"use strict";
sys.setAppName("launcher");

function openApp(name) {
    var apps = sys.apps();
    for (var i = 0; i < apps.length; i++) {
        if (apps[i].name === name) {
            sys.focus(apps[i].slot);
            return;
        }
    }
    var slot = sys.launch(name);
    if (slot >= 0) {
        sys.focus(slot); // launch は同期、focus はキュー経由で直後に届く
    } else {
        sys.notify("起動できません: " + name);
        sys.focus(0); // 行き先を失ったタップの受け皿はランチャー自身
    }
}

sys.onSignal(function (v, from) {
    var req;
    try {
        req = JSON.parse(v);
    } catch (e) {
        return; // open 規約以外のシグナルは無視
    }
    if (req && req.op === "open" && req.app)
        openApp(req.app);
});

function build() {
    /* 停止 (✕) 後の作り直しで retain スタックを太らせない: 一覧は常に
       コンソール直上の 1 枚だけにする */
    while (ui.back()) {}
    var s = ui.screen("アプリ");
    var list = s.list();
    var apps = sys.apps();
    var running = {};
    for (var i = 0; i < apps.length; i++) {
        running[apps[i].name] = true;
        if (apps[i].name === "launcher")
            continue; // 自分は載せない (PC テストでは slot 0 とは限らない)
        (function (app) {
            /* 行タップ = 即切替、行末の ✕ = 即停止 (確認ページなし。
               誤タップしても ○ 行 / チップから 1 タップで復活できる) */
            list.add("● " + app.name + "  (slot " + app.slot + ")",
                     function () { sys.focus(app.slot); },
                     function () {
                         sys.stop(app.slot);
                         build();
                     });
        })(apps[i]);
    }
    /* dev スロット (1) が明示停止で寝ているときだけ再開行を出す
       (動作中はその実名の ● 行が上にある) */
    var devRunning = false;
    for (var k = 0; k < apps.length; k++)
        if (apps[k].slot === 1)
            devRunning = true;
    if (!devRunning)
        list.add("○ dev タスク (push されたタスクを再開)",
                 function () { openApp("dev"); });
    var inst = sys.installed();
    for (var j = 0; j < inst.length; j++) {
        if (!running[inst[j]])
            (function (name) {
                list.add("○ " + name, function () { openApp(name); });
            })(inst[j]);
    }
    s.label("● 実行中 / x で停止 / ○ 停止中 ... バー長押しでいつでもここへ");
}

sys.onForeground(build);

print("launcher ready");
