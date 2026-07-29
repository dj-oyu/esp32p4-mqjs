/* ssh_vt の IME 台本テスト (docs/skk-ime-design.md S6)。
 *
 * これは単体で動くスクリプトではなく、tools/tests/ssh_vt_imetest.sh が
 * examples/ssh_vt.js の "@imetest-inject" マーカー行の位置へ注入してから
 * run_pc に食わせる断片です。ssh_vt のクロージャ (onKey / imeSend /
 * imePre / cands / sessions …) に届く必要があるので、この位置でしか
 * 成立しません。出荷アプリにはこのバイトは 1 つも乗りません。
 *
 * 見ているのは変換の正しさ (それは skk_core のホストテスト S1/S2 の
 * 仕事) ではなく **ssh_vt がキーを渡す順序** — IME フックが TOKSEQ 展開
 * より前にあるか。ui.onKey は run_pc で発火しないので、この網が無いと
 * 順序バグは実機まで誰にも見えない。
 *
 * ⚠️ 順序が実際に効くのは **"\0name" トークン (矢印/esc) だけ** です。
 * skk は K_BS と K_DEL を両方受け、SELECT 中は候補キー以外なら何でも
 * 確定するので、"\n"->"\r" / "\b"->"\x7f" の書き換えを先に通しても変換は
 * 通ってしまう (2026-07-29 にサボタージュして確認)。だから検出点は
 * arrow-swallow で、それが無かった頃のこの台本はフックを TOKSEQ 展開の
 * 後ろへ動かしても PASS していた。同じ穴を開けないこと。
 *
 * ⚠️ 辞書は自前で解決する。run_pc は SKK_HAVE_BUILTIN_IMAGE 無しで
 * ビルドされるので skk.open("") は必ず失敗する。README の run_pc レシピの
 * cwd (components/mqjs) から実物を指すこと。これを放置していた間、台本は
 * 1 行目の open 失敗で全件スキップし、緑のまま何も検証していなかった。 */
    {
        if (!IME_DICT)
            IME_DICT = "../skk_core/skk_dict.bin";
        var sent = [];    /* IME が確定させた文字列 (imeSend 経由) */
        var wrote = [];   /* 素通しでリモートへ行った打鍵 (ssh.write 経由) */
        imeSend = function (idArg, s) { sent.push(s); };
        ssh.write = function (idArg, s) { wrote.push(s); };
        sessions.push({ id: 1, term: makeTerm(), label: "imetest" });
        actIdx = 0;
        var fails = 0;
        var feed = function (a) {
            for (var fi = 0; fi < a.length; fi++)
                onKey(a[fi]);
        };
        var check = function (name, ok) {
            if (ok)
                return;
            fails++;
            print("FAIL " + name + ": mode=" + imeMode + " pre=" +
                  JSON.stringify(imePre) + " cands=" + cands.length +
                  " sel=" + candSel + " sent=" + JSON.stringify(sent) +
                  " wrote=" + JSON.stringify(wrote) +
                  (imeErr ? " err=" + imeErr : ""));
        };
        /* 確定文字列が「辞書から来た日本語」であることの最小判定。書き換えが
           先に走っていると COMMIT 自体が起きない = 順序バグの検出点 */
        var isJa = function (s) {
            return typeof s === "string" && s.length > 0 &&
                   s.charCodeAt(0) > 0x7F;
        };
        feed(["\x00ime"]);
        check("dict", imeReady);
        check("toggle", imeOn);
        if (imeReady) {
            feed(["K", "a", "n", "j", "i"]);
            check("midashi", imeMode === skk.MIDASHI && imePre.length > 0);
            check("midashi-quiet", sent.length === 0); /* 変換前に送っていない */
            feed([" "]);
            check("select", imeMode === skk.SELECT && cands.length > 0);
            var nCand = cands.length;
            feed([" ", " "]);          /* ▼ 連打は SEL だけ (設計 §4.4) */
            check("next", cands.length === nCand &&
                          (candSel > 0 || nCand === 1));
            feed(["\n"]);              /* 生の "\n" のまま skk へ行くこと */
            check("commit", sent.length === 1 && isJa(sent[0]));
            check("commit-pre", imePre === "");
            sent = [];
            feed(["O", "k", "u", "R", "u", "\n"]);
            check("okuri", sent.length === 1 && isJa(sent[0]));
            sent = [];
            feed(["K", "a", "n", "j", "i"]);
            var preBs = imePre;
            feed(["\b"]);              /* "\x7f" に化ける前に届くこと */
            check("bs", imePre.length > 0 && imePre.length < preBs.length &&
                        sent.length === 0);
            /* ⚠️ フックの位置を実際に守っているのはここ。▽ が開いている間の
               矢印は skk が TOK_SWALLOW で飲む (preedit はアプリのカーソル
               位置に描かれるので、カーソルだけ動くと取り残される —
               skk_kana.c の token_of/MIDASHI 分岐)。フックが TOKSEQ 展開の
               **後ろ** へ動くと "\x00left" は "\x1b[D" になってから skk に
               届き、トークンとして認識されずリモートへ漏れる。
               他の項目ではこれを検出できない: skk は K_BS と K_DEL を両方
               受け、SELECT 中は候補キー以外なら何でも確定するので、
               "\n"->"\r" / "\b"->"\x7f" の書き換えを先に通しても通ってしまう
               (2026-07-29 に実際にサボタージュして確認した)。 */
            wrote = [];
            feed(["K", "a", "n"]);
            feed(["\x00left"]);
            check("arrow-swallow", imePre.length > 0 && wrote.length === 0);
            feed(["\x00esc"]);         /* TOKSEQ 展開より前に取消として消費 */
            check("esc", imePre === "" && sent.length === 0 &&
                         wrote.length === 0);
            feed(["\x00ime"]);
            check("off", !imeOn);
            sent = [];
            wrote = [];
            feed(["a", "\n", "\x00up"]);
            /* IME オフの 3 打鍵は素通しでリモートへ行くこと。imeSend を
               通らないだけでなく、本当に ssh.write まで届くかを見る */
            check("passthru", sent.length === 0 && wrote.length === 3);
        }
        print(fails === 0 ? "ssh_vt IME selftest: PASS"
                          : "ssh_vt IME selftest: FAIL x" + fails);
    }
