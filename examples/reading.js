// @app reading
// @title 読書記録
// @icon 
// @desc ページ進捗を記録して XP・連続日数・バッジで読書を後押し。ISBN は NDL サーチ (PC ブリッジ) で自動入力。
// @perm mqtt
//
// 読書記録: 蔵書管理ではなく「読書の支援」のためのアプリ。
//
//  - 本ごとに「どこまで読んだか」を記録し、進捗率を一覧表示
//  - ゲーミフィケーション: 読んだページ = XP、毎日読むと連続日数ボーナス、
//    読了ボーナス、8 種のバッジ (実績画面で確認)
//  - 本の追加は ISBN を入れて NDL サーチから自動入力 (タイトル/著者/
//    ページ数)。本体に HTTP は無いので、PC 側の tools/ndl_bridge.py が
//    MQTT (esp32p4-mqjs/ndl/req → res) で代理検索する。ブリッジ不在でも
//    手入力で全機能が使える (ローカルファースト)
//  - カメラバーコード読取はランタイム未対応 (カメラバインディングなし)。
//    将来 C 側に camera.* が生えたらここに足す
//  - データは NVS (rd_books / rd_game)。microSD 不要、最大 25 冊
//
// 検索機能・書影は意図的に無し (目的は読書支援であって蔵書管理ではない)。
// 連続日数は SNTP で時計が合うまで動かない (壁時計が無意味なため)。
// PC (run_pc) ではゲーミフィケーションと永続化の SELFTEST が走る。
"use strict";
sys.setAppName("reading");

var SELFTEST = false; /* true にすると PC でロジック検証だけ走る */
var HAS_UI = !SELFTEST && ui.size()[0] !== 0;

var BROKER = "mqtt://192.168.1.2";
var REQ_T = "esp32p4-mqjs/ndl/req";
var RES_T = "esp32p4-mqjs/ndl/res";
var MAX_BOOKS = 25; /* NVS 値 3.9KB に収める */

/* ===== モデル ===== */
/* 本 = {t:題, a:著者, tp:総ページ, cp:現在ページ, fd:読了日 (0=未読了)} */
var books = [];
/* ゲーム状態: xp, st=連続日数, bs=最高連続, ld=最終記録日, tp=今日のページ,
 * fin=読了数, bd=バッジ bitmask, pg=累計ページ */
var game = null;

function defGame() {
    return { xp: 0, st: 0, bs: 0, ld: 0, tp: 0, fin: 0, bd: 0, pg: 0 };
}

function save() {
    try {
        store.set("rd_books", JSON.stringify(books));
        store.set("rd_game", JSON.stringify(game));
    } catch (e) {}
}

function load() {
    try {
        var s = store.get("rd_books");
        books = s ? JSON.parse(s) : [];
    } catch (e) {
        books = [];
    }
    game = defGame();
    try {
        var g = store.get("rd_game");
        if (g) {
            var d = JSON.parse(g);
            for (var k in game)
                if (d[k] !== undefined) game[k] = d[k];
        }
    } catch (e2) {}
}

/* ===== ゲーミフィケーション (純粋ロジック、日付は引数で注入) ===== */
function level(xp) { return 1 + Math.floor(Math.sqrt(xp / 100)); }
function lvlBase(lv) { return (lv - 1) * (lv - 1) * 100; }
function lvlNext(lv) { return lv * lv * 100; }

/* JST の通算日。SNTP 同期前 (2020 年より昔) は 0 = 日付機能オフ */
function today() {
    var ms = Date.now();
    if (ms < 1577836800000) return 0;
    return Math.floor((ms + 32400000) / 86400000);
}

var BADGES = [
    { n: "はじめの一冊", d: "本を登録する" },
    { n: "読了デビュー", d: "1 冊読み切る" },
    { n: "三日坊主卒業", d: "3 日連続で読む" },
    { n: "一週間の習慣", d: "7 日連続で読む" },
    { n: "集中読書", d: "1 日に 50 ページ" },
    { n: "ページタービン", d: "1 日に 100 ページ" },
    { n: "千里の道", d: "累計 1000 ページ" },
    { n: "本の虫", d: "5 冊読了" }
];

function checkBadges(msgs) {
    function earn(bit) {
        if (game.bd & (1 << bit)) return;
        game.bd |= (1 << bit);
        msgs.push("バッジ獲得: " + BADGES[bit].n);
    }
    if (books.length >= 1) earn(0);
    if (game.fin >= 1) earn(1);
    if (game.st >= 3) earn(2);
    if (game.st >= 7) earn(3);
    if (game.tp >= 50) earn(4);
    if (game.tp >= 100) earn(5);
    if (game.pg >= 1000) earn(6);
    if (game.fin >= 5) earn(7);
}

/* ページ記録。戻り値 = 表示メッセージ配列。day=0 なら日付機能なし */
function logRead(b, newCp, day) {
    var msgs = [];
    if (newCp > b.tp) newCp = b.tp;
    if (newCp < 0) newCp = 0;
    var delta = newCp - b.cp;
    b.cp = newCp;
    if (delta <= 0) {
        save();
        return ["ページ位置を修正しました (XP なし)"];
    }
    var gain = delta;
    if (day > 0) {
        if (game.ld !== day) {
            game.st = (day === game.ld + 1) ? game.st + 1 : 1;
            if (game.st > game.bs) game.bs = game.st;
            game.ld = day;
            game.tp = 0;
            var sb = game.st * 5;
            if (sb > 25) sb = 25;
            gain += 10 + sb;
            msgs.push("今日はじめての読書 +10XP / 連続 " + game.st + " 日 +" +
                      sb + "XP");
        }
        game.tp += delta;
    }
    game.pg += delta;
    if (b.cp >= b.tp && !b.fd) {
        b.fd = day || 1;
        game.fin++;
        gain += 50;
        msgs.push("読了! ボーナス +50XP");
    }
    var was = level(game.xp);
    game.xp += gain;
    msgs.push("+" + delta + "ページ = +" + gain + "XP (計 " + game.xp + ")");
    if (level(game.xp) > was)
        msgs.push("レベルアップ! Lv" + level(game.xp));
    checkBadges(msgs);
    save();
    return msgs;
}

/* ===== 表示ヘルパ ===== */
function cap(s, n) {
    if (s.length > n) return s.slice(0, n - 1) + "…";
    return s;
}
function bar(cur, max, w) {
    if (max <= 0) max = 1;
    var f = Math.floor(w * cur / max);
    if (f > w) f = w;
    if (f < 0) f = 0;
    var s = "[";
    for (var i = 0; i < w; i++) s += i < f ? "#" : ".";
    return s + "]";
}
function pct(b) { return b.tp > 0 ? Math.floor(100 * b.cp / b.tp) : 0; }

/* ===== 画面 (ウィジェットモード) ===== */
var gen = 0;       /* 画面世代: NDL 応答が古い画面の widget を触らない用 */
var ndlWait = null;
var ndlTimer = 0;  /* 応答タイムアウト (ブリッジ不在をユーザーに見せる) */

/* モデルが変わったら home から作り直す (動的リストの標準パターン) */
function goHome() {
    while (ui.back()) {}
    buildHome();
}

function buildHome() {
    gen++;
    var s = ui.screen("読書記録");
    var lv = level(game.xp);
    s.label("Lv" + lv + " " +
            bar(game.xp - lvlBase(lv), lvlNext(lv) - lvlBase(lv), 10) +
            " " + game.xp + "XP   連続 " + game.st + " 日");
    if (books.length === 0)
        s.label("「本を追加」から最初の 1 冊を登録しましょう");
    var l = s.list();
    for (var i = 0; i < books.length; i++) {
        (function (k) {
            var b = books[k];
            l.add((b.fd ? "済 " : "") + bar(b.cp, b.tp, 10) + " " +
                  pct(b) + "% " + cap(b.t, 15),
                  function () { buildBook(k); });
        })(i);
    }
    s.button("本を追加", buildAdd);
    s.button("実績とバッジ", buildStats);
    s.button("コンソールへ戻る", ui.back);
}

function buildBook(k) {
    gen++;
    var b = books[k];
    var s = ui.screen(cap(b.t, 12));
    s.label(cap(b.t, 24));
    if (b.a) s.label(b.a);
    var prog = s.label("");
    function progText() {
        return bar(b.cp, b.tp, 14) + " " + b.cp + " / " + b.tp +
               "ページ (" + pct(b) + "%)" + (b.fd ? " 読了" : "");
    }
    prog.setText(progText());
    var status = s.label(b.fd ? "読了済み。読み返しも記録できます" :
                               "どこまで読んだ?");
    var f = s.field("現在のページ");
    f.setText("" + b.cp);
    function rec(ncp) {
        var msgs = logRead(b, ncp, today());
        f.setText("" + b.cp);
        prog.setText(progText());
        status.setText(msgs.join(" / "));
        for (var m = 0; m < msgs.length; m++)
            if (msgs[m].indexOf("レベルアップ") >= 0 ||
                msgs[m].indexOf("バッジ") >= 0 ||
                msgs[m].indexOf("読了") >= 0)
                sys.notify(msgs[m]);
    }
    s.button("ここまで読んだ (入力したページで記録)", function () {
        var v = Math.floor(Number(f.value()));
        if (!isFinite(v)) {
            status.setText("ページ数を数字で入れてください");
            return;
        }
        rec(v);
    });
    s.button("+10 ページ", function () { rec(b.cp + 10); });
    s.button("+25 ページ", function () { rec(b.cp + 25); });
    s.button("この本を削除", function () {
        books.splice(k, 1);
        save();
        goHome();
    });
    s.button("戻る", goHome);
}

function buildAdd() {
    gen++;
    var myGen = gen;
    var s = ui.screen("本を追加");
    var status = s.label("ISBN から自動入力するか、手で入れてください");
    var fIsbn = s.field("ISBN");
    var fTitle = s.field("タイトル (必須)");
    var fAuthor = s.field("著者");
    var fPages = s.field("総ページ数 (必須)");
    s.button("NDL サーチで自動入力 (要 PC ブリッジ)", function () {
        var isbn = (fIsbn.value() || "").replaceAll("-", "").replaceAll(" ", "");
        if (isbn.length < 10) {
            status.setText("ISBN (10 桁か 13 桁) を入れてください");
            return;
        }
        if (!mqtt.connected()) {
            status.setText("ブローカー未接続: 手入力してください");
            return;
        }
        ndlWait = { gen: myGen, isbn: isbn, status: status,
                    t: fTitle, a: fAuthor, p: fPages };
        mqtt.publish(REQ_T, isbn);
        status.setText("NDL サーチに問い合わせ中...");
        if (ndlTimer) clearTimeout(ndlTimer);
        ndlTimer = setTimeout(function () {
            ndlTimer = 0;
            if (!ndlWait || ndlWait.gen !== gen) return;
            ndlWait.status.setText(
                "応答なし: PC で tools/ndl_bridge.py が動いているか確認 (手入力も可)");
            ndlWait = null;
        }, 10000);
    });
    s.button("追加", function () {
        if (books.length >= MAX_BOOKS) {
            status.setText("登録は " + MAX_BOOKS + " 冊まで (読了本を削除してください)");
            return;
        }
        var t = fTitle.value();
        var tp = Math.floor(Number(fPages.value()));
        if (!t) { status.setText("タイトルを入れてください"); return; }
        if (!(tp > 0)) {
            status.setText("総ページ数を正の数で入れてください");
            return;
        }
        books.push({ t: cap(t, 40), a: cap(fAuthor.value() || "", 24),
                     tp: tp, cp: 0, fd: 0 });
        var msgs = [];
        checkBadges(msgs);
        save();
        for (var m = 0; m < msgs.length; m++) sys.notify(msgs[m]);
        goHome();
    });
    s.button("キャンセル", goHome);
}

function buildStats() {
    gen++;
    var s = ui.screen("実績とバッジ");
    var lv = level(game.xp);
    s.label("レベル " + lv + "   " + game.xp + " XP");
    s.label(bar(game.xp - lvlBase(lv), lvlNext(lv) - lvlBase(lv), 14) +
            " 次のレベルまで " + (lvlNext(lv) - game.xp) + " XP");
    s.label("連続記録: " + game.st + " 日 (最高 " + game.bs + " 日)");
    s.label("今日のページ: " +
            (today() > 0 && game.ld === today() ? game.tp : 0));
    s.label("累計 " + game.pg + " ページ / 読了 " + game.fin + " 冊");
    var l = s.list();
    for (var i = 0; i < BADGES.length; i++) {
        var got = (game.bd & (1 << i)) !== 0;
        l.add((got ? "● " : "○ ") + BADGES[i].n + " - " + BADGES[i].d,
              function () {});
    }
    s.button("戻る", goHome);
}

/* ===== NDL ブリッジ応答 ===== */
function ndlReply(t, payload) {
    if (!ndlWait || ndlWait.gen !== gen) { ndlWait = null; return; }
    var m;
    try { m = JSON.parse(payload); } catch (e) { return; }
    if (!m || m.isbn !== ndlWait.isbn) return;
    if (ndlTimer) { clearTimeout(ndlTimer); ndlTimer = 0; }
    if (!m.ok) {
        ndlWait.status.setText("NDL: 見つかりませんでした。手入力してください");
        ndlWait = null;
        return;
    }
    if (m.title) ndlWait.t.setText(cap(m.title, 40));
    if (m.author) ndlWait.a.setText(cap(m.author, 24));
    if (m.pages) ndlWait.p.setText("" + m.pages);
    ndlWait.status.setText("取得: " + cap(m.title || "?", 18) +
                           (m.pages ? " (" + m.pages + "p)"
                                    : " (ページ数は手入力)"));
    ndlWait = null;
}

/* ===== SELFTEST (PC run_pc: UI なしのとき) ===== */
function selftest() {
    var fails = 0;
    function ok(cond, name) {
        if (cond) print("ok " + name);
        else { fails++; print("FAIL " + name); }
    }

    game = defGame();
    books = [];
    var b = { t: "テスト本", a: "", tp: 100, cp: 0, fd: 0 };
    books.push(b);
    var msgs = [];
    checkBadges(msgs);
    ok((game.bd & 1) !== 0, "badge first-book");

    logRead(b, 30, 100); /* day 100: 30p + 10 daily + 5 streak1 */
    ok(game.xp === 45, "xp day1: " + game.xp);
    ok(game.st === 1 && game.tp === 30, "streak/today day1");

    logRead(b, 40, 100); /* 同日 +10p のみ */
    ok(game.xp === 55, "xp same day: " + game.xp);
    ok(game.st === 1 && game.tp === 40, "streak/today same day");

    logRead(b, 50, 101); /* 翌日: 10p + 10 + streak2*5 */
    ok(game.st === 2, "streak 2");
    ok(game.xp === 85, "xp day2: " + game.xp);

    logRead(b, 60, 102); /* 3 日目 → バッジ */
    ok(game.st === 3, "streak 3");
    ok((game.bd & 4) !== 0, "badge 3days");
    ok(game.xp === 120, "xp day3: " + game.xp);

    logRead(b, 100, 105); /* 中断→リセット + 読了: 40p+10+5+50 */
    ok(game.st === 1, "streak reset");
    ok(b.fd === 105 && game.fin === 1, "finished");
    ok((game.bd & 2) !== 0, "badge finished");
    ok(game.xp === 225, "xp finish: " + game.xp);

    var x0 = game.xp;
    logRead(b, 90, 106); /* 後退は修正扱い (XP なし) */
    ok(game.xp === x0 && b.cp === 90, "correction no xp");

    logRead(b, 5000, 107); /* 総ページでクランプ */
    ok(b.cp === 100, "clamp at tp");

    ok(level(0) === 1 && level(99) === 1 && level(100) === 2 &&
       level(399) === 2 && level(400) === 3, "level curve");
    ok(lvlNext(1) === 100 && lvlBase(2) === 100, "level bounds");

    ok(bar(5, 10, 10) === "[#####.....]", "bar render");
    ok(pct({ cp: 45, tp: 90 }) === 50, "pct");

    /* 永続化ラウンドトリップ (PC の store はセッション内テーブル) */
    save();
    var keep = JSON.stringify({ b: books, g: game });
    books = [];
    game = defGame();
    load();
    ok(JSON.stringify({ b: books, g: game }) === keep, "persist roundtrip");

    if (fails) throw new Error("reading selftest: " + fails + " failed");
    print("reading selftest: ALL PASS");
}

/* ===== 起動 ===== */
if (HAS_UI) {
    load();
    mqtt.subscribe(RES_T, ndlReply);
    mqtt.connect(BROKER); /* 日和見: ブローカー不在でも手入力で全機能可 */
    buildHome();
    sys.onForeground(goHome);
} else {
    selftest();
}
