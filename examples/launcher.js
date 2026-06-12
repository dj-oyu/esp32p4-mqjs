// ランチャー (P4b): 常駐の組み込みアプリ (kind: "system")。
//
// - アプリ一覧: 実行中 (●) はタップで即切替・行末の ✕ で即停止、
//   停止中 (○) はタップで起動 (= 開く)。dev タスクの再開行もここ。
//   確認ページは置かない: 停止の被害は「消える」だけで、○ 行 /
//   ステータスバーのチップから 1 タップで復活できる。
// - 「開く」要求の解決役: ステータスバーのチップ/通知タップは
//   {"op":"open","app":<name>} のシグナルとしてここに届く。sys.open が
//   動いていれば focus、止まっていれば起動 → focus を一括で行う
//   (差出人は問わない: アプリからの sys.signal("launcher", ...) でも同じ)。
// - C 側の不変条件によりランチャーは停止不可・死んでも自動再起動される。
//
// App Manager 移行 Phase 1: アプリの公開識別子は名前。slot 番号は
// このファイルでは使わない (sys.focus/stop/open すべて名前で呼ぶ)。
// 画面は他アプリ同様「切替時に破棄、onForeground で再構築」。
"use strict";
sys.setAppName("launcher");

function openApp(name) {
    if (!sys.open(name)) {
        sys.notify("起動できません: " + name);
        sys.focus("launcher"); // 行き先を失ったタップの受け皿は自分
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

/* ---- §9 ストア: インストール済み (= 棚と同期した littlefs) の閲覧。
   メインのスイッチャー役を毀損しないよう ui.screen のサブページで
   増築 (retain スタック: メイン → ストア → 詳細 = 深さ 3 で枠内)。 */
function storePage() {
    var s = ui.screen("ストア");
    var inst = sys.installed();
    var apps = sys.apps();
    var running = {};
    for (var i = 0; i < apps.length; i++)
        running[apps[i].name] = true;
    if (!inst.length)
        s.label("インストール済みアプリはありません");
    var list = s.list();
    for (var j = 0; j < inst.length; j++) {
        (function (it) {
            var label = (running[it.name] ? "● " : "○ ") +
                        (it.icon ? it.icon + " " : "") + it.title;
            list.add(label, function () { detailPage(it, !!running[it.name]); });
        })(inst[j]);
    }
    /* §11: 棚にあってこの端末に入っていないアプリ (カタログ由来)。
       インストールは要求のみ非同期 — 完了は "installed:" 通知で届き、
       次にこのページを開いたとき済み側に並ぶ。 */
    var store = sys.store();
    var have = {};
    for (var k = 0; k < inst.length; k++)
        have[inst[k].name] = true;
    var avail = [];
    for (var m = 0; m < store.length; m++)
        if (!store[m].installed && !have[store[m].name])
            avail.push(store[m]);
    if (avail.length) {
        s.label("入手可能 (" + avail.length + ")");
        var al = s.list();
        for (var a = 0; a < avail.length; a++) {
            (function (it) {
                al.add((it.icon ? it.icon + " " : "") + it.title,
                       function () { availPage(it); });
            })(avail[a]);
        }
    }
    s.label("行タップで詳細 (説明 / 権限 / 起動)");
    s.button("戻る", function () { ui.back(); });
}

/* 未インストール詳細: マニフェスト情報と [インストール]。要求が通ると
   署名検証済みの本体が registry 経路で入る (自動実行はしない §6)。 */
function availPage(it) {
    var s = ui.screen((it.icon ? it.icon + " " : "") + it.title);
    s.label("name: " + it.name);
    if (it.desc)
        s.label(it.desc);
    if (it.perm)
        s.label("権限 (宣言のみ): " + it.perm);
    if (it.size)
        s.label("サイズ: " + it.size + " B");
    s.button("インストール", function () {
        if (sys.install(it.name))
            sys.notify("インストール要求: " + it.name);
        else
            sys.notify("要求できません (ブローカー未接続?)");
        ui.back(); /* 完了は installed: 通知 → ストアを開き直すと済み側 */
    });
    s.button("戻る", function () { ui.back(); });
}

function detailPage(it, isRunning) {
    var s = ui.screen((it.icon ? it.icon + " " : "") + it.title);
    s.label("name: " + it.name + (isRunning ? "  (実行中)" : ""));
    if (it.desc)
        s.label(it.desc);
    if (it.perm)
        s.label("権限 (宣言のみ): " + it.perm);
    s.label("サイズ: " + it.size + " B");
    if (it.autostart)
        s.label(it.optin ? "自動起動: 有効 (起動時に常駐)"
                         : "自動起動: 宣言あり (一度起動すると有効化)");
    s.button(isRunning ? "前面へ" : "起動して開く", function () {
        openApp(it.name); /* 既存の open 経路: focus-or-launch */
    });
    s.button("アンインストール", function () {
        sys.uninstall(it.name);
        /* §11: 購読も外れるので retained 本体では復活しない。棚に
           あればストアの「入手可能」に並び直す (再インストール可) */
        sys.notify("削除: " + it.name + " (ストアから再インストール可)");
        build();
    });
    s.button("戻る", function () { ui.back(); });
}

function build() {
    /* 停止 (✕) 後の作り直しで retain スタックを太らせない: 一覧は常に
       コンソール直上の 1 枚だけにする */
    while (ui.back()) {}
    var s = ui.screen("アプリ");
    var list = s.list();
    var apps = sys.apps();
    var inst = sys.installed(); /* P4c: [{name, title, perm, icon}] */
    var icons = {};
    for (var j0 = 0; j0 < inst.length; j0++)
        icons[inst[j0].name] = inst[j0].icon || "";
    var running = {};
    for (var i = 0; i < apps.length; i++) {
        running[apps[i].name] = true;
        if (apps[i].name === "launcher")
            continue; // 自分は載せない (PC テストでは slot 0 とは限らない)
        (function (app) {
            /* 行タップ = 即切替、行末の ✕ = 即停止 (確認ページなし。
               誤タップしても ○ 行 / チップから 1 タップで復活できる) */
            var ic = icons[app.name] ? icons[app.name] + " " : "";
            list.add("● " + ic + app.name,
                     function () { sys.focus(app.name); },
                     function () {
                         sys.stop(app.name);
                         build();
                     });
        })(apps[i]);
    }
    /* dev タスクが明示停止で寝ているときだけ再開行を出す
       (動作中はその実名の ● 行が上にある) */
    var devRunning = false;
    for (var k = 0; k < apps.length; k++)
        if (apps[k].kind === "dev")
            devRunning = true;
    if (!devRunning)
        list.add("○ dev タスク (push されたタスクを再開)",
                 function () { openApp("dev"); });
    /* §9: ○ (停止中) の一覧はストアページへ移動 — メインは
       スイッチャー専念。停止中アプリはストア経由かチップで開く。 */
    s.button("ストア (インストール済み " + inst.length + " 本)", storePage);
    s.label("● タップで切替 / x で停止 ... バー長押しでいつでもここへ");

    /* P4c: アプリ毎の最終通知。タップで発信アプリを開く (停止中なら
       起動して開く — open 経路がそのまま面倒を見る) */
    var nts = sys.notices();
    if (nts.length) {
        s.label(" 通知"); /* nf-fa-bell (NF 連鎖で全テキスト面に出る) */
        var nl = s.list();
        for (var k2 = 0; k2 < nts.length; k2++) {
            (function (nt) {
                nl.add(nt.app + ": " + nt.text,
                       function () { openApp(nt.app); });
            })(nts[k2]);
        }
    }
}

sys.onForeground(build);

print("launcher ready");
