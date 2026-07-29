/* OLD-PATH trace: injected into examples/ssh_vt.js at @imetest-inject and
   run under run_pc with the real dictionary. Emits one line per key so the
   ime_core driver can be diffed against it. Throwaway. */
    {
        if (!IME_DICT)
            IME_DICT = "../skk_core/skk_dict.bin";
        var KEYS = [
            "\x00ime",
            "a", "i",
            "K", "a", "n", "j", "i",
            " ", " ", "\n",
            "O", "k", "u", "R", "u", "\n",
            "K", "a", "n",
            "\b",
            "\x00left",
            "\x00esc",
            "\x00up",
            "\x00ime",
            "a", "\n", "\x00up"
        ];
        var sent = [], wrote = [];
        imeSend = function (idArg, s) { sent.push(s); };
        ssh.write = function (idArg, s) { wrote.push(s); };
        sessions.push({ id: 1, term: makeTerm(), label: "diff" });
        actIdx = 0;
        print("DICT ?");
        for (var i = 0; i < KEYS.length; i++) {
            var ns = sent.length, nw = wrote.length;
            onKey(KEYS[i]);
            var cls = sent.length > ns ? "TEXT"
                    : (wrote.length > nw ? "PASS" : "TAKEN");
            var txt = sent.length > ns ? sent[sent.length - 1] : "";
            print("T " + i + " " + cls + " pre=" + imePre + " mode=" + imeMode +
                  " nc=" + cands.length + " txt=" + txt);
        }
        print("READY " + (imeReady ? "1" : "0") + " ERR " + (imeErr || "-"));
        print("TRACE END");
    }
