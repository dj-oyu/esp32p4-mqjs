// @app files
// @title ファイラ
// @icon 
// @desc 内蔵ストレージと microSD を 1 つの画面で扱う
//
// 設計は docs/filer-storage-design.md。このアプリはボードを一度も
// 知らない: fs.volumes() が返した本数だけ入口を並べるので、microSD の
// 無い Stamp-P4 では「内蔵」1 本の画面になるだけで、分岐は 1 行も無い。
//
// 画面は常に 1 枚 (retain 深さ 3 の枠内) にして、階層は JS 側の cwd で
// 持つ。ディレクトリを降りるたびに画面を積むと、5 階層で枠を割る。
"use strict";
sys.setAppName("files");

var cwd = "";        // "" = ボリューム一覧、それ以外は "/sd/DCIM" 等
var grants = {};     // ボリューム id -> 書き込み grant (0/未設定 = 無し)
var clip = null;     // {op:"copy"|"move", path:..., name:...}
var msg = "";        // 次の描画で 1 度だけ出す一行

/* mquickjs はブロック内の function 宣言を巻き上げないので、この
   ファイルの関数はすべて var 束縛で書く (examples/README の落とし穴)。 */
var render, dirPage, volumesPage;

var volOf = function (p) {
    var i = p.indexOf("/", 1);
    return i < 0 ? p.slice(1) : p.slice(1, i);
};

var nameOf = function (p) {
    var i = p.lastIndexOf("/");
    return i < 0 ? p : p.slice(i + 1);
};

var parentOf = function (p) {
    var i = p.lastIndexOf("/");
    if (i <= 0)
        return "";               // "/sd" の親はボリューム一覧
    return p.slice(0, i);
};

var join = function (dir, name) {
    return dir + "/" + name;
};

var fmtSize = function (n) {
    if (n === undefined || n === null)
        return "";
    if (n < 1024)
        return n + " B";
    if (n < 1024 * 1024)
        return (n / 1024).toFixed(1) + " KB";
    if (n < 1024 * 1024 * 1024)
        return (n / 1024 / 1024).toFixed(1) + " MB";
    return (n / 1024 / 1024 / 1024).toFixed(2) + " GB";
};

/* ---- 権限 --------------------------------------------------------
   grant はアプリが止まるかカードが抜けると自動的に死ぬ。死んだものを
   使うと例外になるので、掴み直しは「失敗したら捨てて訊き直す」だけで
   済む — 有効期限を自前で数えない。 */

var askGrant = function (volId, reason, then) {
    var ok;
    try {
        /* カードが抜けていれば fs.request 自体が投げる (尋ねる意味が
           無いので C 側で先に弾いている)。 */
        ok = fs.request({ path: "/" + volId, write: true, reason: reason },
                        function (g) {
            if (!g) {
                msg = "許可されませんでした";
                render();
                return;
            }
            grants[volId] = g;
            then(g);
        });
    } catch (e) {
        msg = "" + e;
        render();
        return;
    }
    if (!ok) {
        msg = "許可を求められませんでした";
        render();
    }
};

/* volId への書き込みを伴う処理。grant が無ければ同意画面を挟んでから
   走る。fn の中で投げられた例外は 1 行のメッセージにして画面に出す。 */
var withGrant = function (volId, reason, fn) {
    var retried = false;
    var run = function (g) {
        try {
            fn(g);
        } catch (e) {
            var s = "" + e;
            /* 期限切れ (アプリ再起動 / カード抜き差し) は一度だけ訊き直す。
               無条件に繰り返すと、断られ続ける状況で同意画面が無限に
               出る — 再試行の上限は 1 回で足りる。 */
            if (!retried && s.indexOf("grant") >= 0) {
                retried = true;
                grants[volId] = 0;
                askGrant(volId, reason, run);
                return;
            }
            msg = s;
        }
        render();
    };
    if (grants[volId])
        run(grants[volId]);
    else
        askGrant(volId, reason, run);
};

/* ---- ボリューム一覧 ---------------------------------------------- */

volumesPage = function () {
    var s = ui.screen("ファイラ");
    if (msg) {
        s.label(msg);
        msg = "";
    }
    var vols = fs.volumes();
    if (!vols.length) {
        s.label("使えるストレージがありません");
        return;
    }
    var list = s.list();
    for (var i = 0; i < vols.length; i++) {
        (function (v) {
            var line;
            if (v.mounted)
                line = v.label + "   " + fmtSize(v.total - v.free) +
                       " / " + fmtSize(v.total);
            else
                line = v.label + "   (入っていません)";
            list.add(line, function () {
                if (!v.mounted) {
                    /* マウントは何も壊さないので grant 不要 */
                    try {
                        fs.mount(v.id);
                    } catch (e) {
                        msg = "" + e;
                    }
                    render();
                    return;
                }
                cwd = v.path;
                render();
            });
        })(vols[i]);
    }
    for (var j = 0; j < vols.length; j++) {
        (function (v) {
            if (!v.removable || !v.mounted)
                return;
            /* 取り出しは書き込みと同じ重さ (他アプリの書き込み中に
               外せる) なので、そのボリュームの grant を要求する。 */
            s.button(v.label + " を取り出す", function () {
                withGrant(v.id, "安全に取り出す", function (g) {
                    fs.unmount(g, v.id);
                    grants[v.id] = 0;
                    msg = v.label + " を取り出しました";
                });
            });
        })(vols[j]);
    }
    if (clip)
        s.label("保留中: " + (clip.op === "copy" ? "コピー" : "移動") +
                " " + clip.name);
};

/* ---- ディレクトリ ------------------------------------------------ */

var mkdirPage = function () {
    var s = ui.screen("新しいフォルダ");
    s.label(cwd + " の中に作ります");
    var f = s.field("名前", "ja");
    s.button("作成", function () {
        var name = f.value();
        if (!name) {
            msg = "名前が空です";
            render();
            return;
        }
        withGrant(volOf(cwd), "フォルダを作る", function (g) {
            fs.mkdir(g, join(cwd, name));
            msg = "作成: " + name;
        });
    });
    s.button("やめる", function () { render(); });
};

var renamePage = function (path) {
    var s = ui.screen("名前の変更");
    s.label(path);
    var f = s.field("新しい名前", "ja");
    f.setText(nameOf(path));
    s.button("変更", function () {
        var name = f.value();
        if (!name || name.indexOf("/") >= 0) {
            msg = "名前に / は使えません";
            render();
            return;
        }
        withGrant(volOf(path), "名前を変える", function (g) {
            fs.rename(g, path, join(parentOf(path), name));
            msg = "変更: " + name;
        });
    });
    s.button("やめる", function () { render(); });
};

/* 貼り付け。コピーはディレクトリを跨げるが、C 側の fs.copy は
   ファイル 1 本ずつなので、フォルダ丸ごとはここでは扱わない。 */
var paste = function () {
    if (!clip)
        return;
    var src = clip.path;
    var dst = join(cwd, clip.name);
    if (src === dst) {
        msg = "同じ場所です";
        render();
        return;
    }
    var op = clip.op;
    withGrant(volOf(cwd), op === "copy" ? "ファイルをコピーする"
                                        : "ファイルを移動する",
        function (g) {
            if (op === "move" && volOf(src) === volOf(cwd)) {
                fs.rename(g, src, dst);
            } else {
                fs.copy(g, src, dst);
                if (op === "move") {
                    /* 移動元は別ボリュームかもしれない: そちらの
                       grant を別に取る (コピーは既に済んでいるので、
                       断られても原本が残るだけで壊れない)。 */
                    var sv = volOf(src);
                    withGrant(sv, "移動元を削除する", function (g2) {
                        fs.remove(g2, src, false);
                        msg = "移動しました";
                        clip = null;
                    });
                    return;
                }
            }
            msg = op === "copy" ? "コピーしました" : "移動しました";
            clip = null;
        });
};

var previewPage = function (path) {
    var s = ui.screen(nameOf(path));
    var text = "";
    try {
        text = fs.read(path, { length: 4096 });
    } catch (e) {
        s.label("読めません: " + e);
        s.button("戻る", function () { render(); });
        return;
    }
    var lines = text.split("\n");
    var shown = lines.length > 40 ? 40 : lines.length;
    for (var i = 0; i < shown; i++)
        s.label(lines[i] === "" ? " " : lines[i]);
    if (lines.length > shown || text.length >= 4096)
        s.label("... (先頭 4KB のみ)");
    s.button("戻る", function () { render(); });
};

var filePage = function (path, size) {
    var s = ui.screen(nameOf(path));
    s.label(parentOf(path));
    s.label("サイズ: " + fmtSize(size));
    var isText = /\.(txt|js|json|md|csv|log|ini|cfg)$/i.test(path);
    if (isText)
        s.button("中身を見る", function () { previewPage(path); });
    s.button("コピー", function () {
        clip = { op: "copy", path: path, name: nameOf(path) };
        msg = "コピー元にしました";
        render();
    });
    s.button("移動", function () {
        clip = { op: "move", path: path, name: nameOf(path) };
        msg = "移動元にしました";
        render();
    });
    s.button("名前の変更", function () { renamePage(path); });
    if (/\.js$/i.test(path))
        s.label("(アプリとして入れるには MQTT 経由の push が要ります)");
    s.button("削除", function () {
        withGrant(volOf(path), "ファイルを削除する", function (g) {
            fs.remove(g, path, false);
            msg = "削除: " + nameOf(path);
            if (clip && clip.path === path)
                clip = null;
        });
    });
    s.button("戻る", function () { render(); });
};

var dirActionsPage = function (path) {
    var s = ui.screen(nameOf(path));
    s.label(path);
    s.button("開く", function () {
        cwd = path;
        render();
    });
    s.button("名前の変更", function () { renamePage(path); });
    s.button("中身ごと削除", function () {
        withGrant(volOf(path), "フォルダを中身ごと削除する", function (g) {
            fs.remove(g, path, true);
            msg = "削除: " + nameOf(path);
        });
    });
    s.button("戻る", function () { render(); });
};

dirPage = function () {
    var s = ui.screen(cwd);
    if (msg) {
        s.label(msg);
        msg = "";
    }
    var items;
    try {
        items = fs.list(cwd);
    } catch (e) {
        /* カードを抜かれた / そのボリュームがこのボードに無い */
        s.label("開けません: " + e);
        s.button("ストレージ一覧へ", function () {
            cwd = "";
            render();
        });
        return;
    }
    if (!items.length)
        s.label("(空)");
    var list = s.list();
    for (var i = 0; i < items.length; i++) {
        (function (it) {
            var p = join(cwd, it.name);
            if (it.dir) {
                list.add("[ " + it.name + " ]", function () {
                    cwd = p;
                    render();
                }, function () { dirActionsPage(p); });
            } else {
                list.add(it.name + "   " + fmtSize(it.size), function () {
                    filePage(p, it.size);
                });
            }
        })(items[i]);
    }
    if (items.more)
        s.label("項目が多すぎて全部は出せていません (" + items.total + " 件)");

    s.button("上へ", function () {
        cwd = parentOf(cwd);
        render();
    });
    s.button("新しいフォルダ", mkdirPage);
    if (clip)
        s.button("ここに" + (clip.op === "copy" ? "コピー" : "移動") +
                 ": " + clip.name, paste);
    if (clip)
        s.button("やめる (" + clip.name + ")", function () {
            clip = null;
            render();
        });
    s.label("フォルダ行の x = 名前変更/削除");
};

render = function () {
    /* 画面は常に 1 枚。階層は cwd が持つので、深く潜っても retain
       スタックは伸びない。 */
    while (ui.back()) {}
    if (!cwd)
        volumesPage();
    else
        dirPage();
};

sys.onForeground(render);
sys.onStop(function () {
    /* 権限は返しておく。返さなくてもアプリの世代が変わった時点で
       死ぬが、席を空けておく方が素直。 */
    for (var k in grants) {
        try {
            fs.release(grants[k]);
        } catch (e) {
        }
    }
});

render();
print("files ready");
