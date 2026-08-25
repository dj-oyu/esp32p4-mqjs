// Firmware-embedded device settings system app.
// Not installed, updated, or replaced through the MQTT app store.
"use strict";

sys.setAppName("device_settings");

function unwind() {
    while (ui.back()) {}
}

// --- Provisioning QR (MQJSP1) parse / validate -----------------------------
// Wire format: "MQJSP1:" + base64url(canonical-json-without-padding).
// Mirrors tools/mqjs_provision_qr.py validate_payload(). Errors carry only
// field names, never decoded secret values.

var QR_PREFIX = "MQJSP1:";
var B64URL = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

function base64urlDecode(str) {
    var bytes = [];
    var buffer = 0;
    var bits = 0;
    for (var i = 0; i < str.length; i++) {
        var idx = B64URL.indexOf(str.charAt(i));
        if (idx < 0) throw new Error("不正なBase64文字");
        buffer = (buffer << 6) | idx;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            bytes.push((buffer >> bits) & 0xff);
        }
    }
    return bytes;
}

function utf8Decode(bytes) {
    var out = "";
    var i = 0;
    while (i < bytes.length) {
        var b0 = bytes[i++];
        var cp;
        if (b0 < 0x80) {
            cp = b0;
        } else if ((b0 & 0xe0) === 0xc0) {
            cp = ((b0 & 0x1f) << 6) | (bytes[i++] & 0x3f);
        } else if ((b0 & 0xf0) === 0xe0) {
            cp = ((b0 & 0x0f) << 12) | ((bytes[i++] & 0x3f) << 6) |
                 (bytes[i++] & 0x3f);
        } else {
            cp = ((b0 & 0x07) << 18) | ((bytes[i++] & 0x3f) << 12) |
                 ((bytes[i++] & 0x3f) << 6) | (bytes[i++] & 0x3f);
        }
        if (cp > 0xffff) {
            cp -= 0x10000;
            out += String.fromCharCode(0xd800 + (cp >> 10), 0xdc00 + (cp & 0x3ff));
        } else {
            out += String.fromCharCode(cp);
        }
    }
    return out;
}

function utf8Len(s) {
    var n = 0;
    for (var i = 0; i < s.length; i++) {
        var c = s.charCodeAt(i);
        if (c < 0x80) n += 1;
        else if (c < 0x800) n += 2;
        else if (c >= 0xd800 && c <= 0xdbff) { n += 4; i++; } // surrogate pair
        else n += 3;
    }
    return n;
}

function isPlainObject(o) {
    return o !== null && typeof o === "object" && !Array.isArray(o);
}

function validatePayload(p) {
    var allowed = ["v", "id", "exp", "device", "wifi", "tailscale"];
    var keys = Object.keys(p);
    for (var i = 0; i < keys.length; i++)
        if (allowed.indexOf(keys[i]) < 0)
            throw new Error("未知のフィールド: " + keys[i]);

    if (p.v !== 1) throw new Error("バージョンが不正です (v=1のみ)");

    if (p.id !== undefined &&
        (typeof p.id !== "string" || p.id.length < 1 || p.id.length > 64))
        throw new Error("idが不正です");
    if (p.exp !== undefined &&
        (typeof p.exp !== "number" || p.exp <= 0 || p.exp % 1 !== 0))
        throw new Error("expが不正です");
    if (p.device !== undefined &&
        (typeof p.device !== "string" || p.device.length < 1 ||
         p.device.length > 63))
        throw new Error("deviceが不正です");

    var wifi = p.wifi;
    var ts = p.tailscale;
    if (wifi === undefined && ts === undefined)
        throw new Error("wifiまたはtailscaleが必要です");

    if (wifi !== undefined) {
        if (!isPlainObject(wifi) || Object.keys(wifi).length !== 2 ||
            wifi.ssid === undefined || wifi.password === undefined)
            throw new Error("wifiはssidとpasswordのみ");
        if (typeof wifi.ssid !== "string" ||
            utf8Len(wifi.ssid) < 1 || utf8Len(wifi.ssid) > 32)
            throw new Error("wifi.ssidが不正です");
        if (typeof wifi.password !== "string" || utf8Len(wifi.password) > 64)
            throw new Error("wifi.passwordが不正です");
    }
    if (ts !== undefined) {
        if (!isPlainObject(ts) || Object.keys(ts).length !== 1 ||
            ts.authKey === undefined)
            throw new Error("tailscaleはauthKeyのみ");
        if (typeof ts.authKey !== "string" ||
            utf8Len(ts.authKey) < 1 || utf8Len(ts.authKey) > 255)
            throw new Error("tailscale.authKeyが不正です");
    }
}

function parseProvisioning(text) {
    text = String(text).trim();
    if (text.indexOf(QR_PREFIX) !== 0)
        throw new Error("MQJSP1ではありません");
    var body = text.slice(QR_PREFIX.length);
    if (!body) throw new Error("空のペイロード");
    var json = utf8Decode(base64urlDecode(body));
    var payload;
    try {
        payload = JSON.parse(json);
    } catch (e) {
        // never surface the parser message: it may quote decoded secrets.
        throw new Error("ペイロードを解析できませんでした");
    }
    if (!isPlainObject(payload))
        throw new Error("JSONオブジェクトではありません");
    validatePayload(payload);
    return payload;
}

// best-effort: wall clock is only valid after SNTP, which has not happened
// yet during first-boot provisioning, so an unsynced clock is "unknown".
function expiryInfo(p) {
    if (p.exp === undefined) return { expired: false, note: "" };
    var nowMs = Date.now();
    var nowSec = Math.floor(nowMs / 1000);
    var CLOCK_VALID_AFTER = 1672531200; // 2023-01-01
    if (nowSec < CLOCK_VALID_AFTER)
        return { expired: false, note: "（時刻未同期のため確認不可）" };
    if (nowSec > p.exp)
        return { expired: true, note: "（⚠ 期限切れ）" };
    return { expired: false, note: "（有効）" };
}

// --- Tailscale live status UI ---------------------------------------------
var tsTimer = 0;
function clearTsTimer() {
    if (tsTimer) { clearInterval(tsTimer); tsTimer = 0; }
}
function tsShort(st) {
    if (st.state === "connected") return "接続済み";
    if (st.state === "connecting") return "接続中";
    if (st.state === "error") return "エラー";
    if (st.state === "disabled") return "オフ";
    return "未設定";
}
function tsStatusLine(st) {
    var line = st.detail || tsShort(st);
    if (st.state === "connected" && st.peers)
        line += " / peers " + st.peers;
    return line;
}

// --- Battery ---------------------------------------------------------------
// The setters here are system-only APIs (power.charge / limit / fullOnce /
// off / sign); this screen is where they belong. Reading is open to any app.

var battTimer = 0;
function clearBattTimer() {
    if (battTimer) { clearInterval(battTimer); battTimer = 0; }
}

function battStateJa(st) {
    if (st === "charging") return "充電中";
    if (st === "discharging") return "放電中";
    if (st === "full") return "満充電";
    if (st === "limited") return "上限で停止";
    if (st === "none") return "電池なし";
    return "不明";
}

function battEtaJa(min) {
    if (min < 0) return "";
    var h = Math.floor(min / 60);
    var m = min % 60;
    if (h > 0) return "  残り約 " + h + "時間" + m + "分";
    return "  残り約 " + m + "分";
}

function battShort() {
    var b = power.battery();
    if (!b.ok || b.pct < 0) return battStateJa(b.state);
    return b.pct + "%  " + battStateJa(b.state);
}

function battLine(b) {
    if (!b.ok) return "バッテリー情報を取得できません";
    if (b.state === "none") return "電池が入っていません (USB給電)";
    var pct = b.pct < 0 ? "--" : ("" + b.pct);
    return pct + "%  " + battStateJa(b.state) + battEtaJa(b.eta);
}

function battDetail(b) {
    // Everything the bring-up needs in one line: terminal voltage, the
    // IR-compensated open-circuit voltage the protection ladder actually
    // uses, current, the learned pack resistance and capacity, and the raw
    // expander input register (bit 6 is the charger/USB-C status pin whose
    // polarity is not documented).
    return (b.mv / 1000).toFixed(2) + "V (ocv " + (b.ocv / 1000).toFixed(2) +
           "V)  " + b.ma + "mA  " + b.mohm + "mΩ" + "\n" +
           b.mah + "/" + b.cap + "mAh  raw=0x" + b.raw.toString(16) +
           "  n=" + b.n;
}

function batteryPage() {
    clearTsTimer();
    clearBattTimer();
    var s = ui.screen("バッテリー");
    var b = power.battery();
    var head = s.label(battLine(b));
    var detail = s.label(battDetail(b));

    s.label("充電の上限 (電池を長持ちさせます)");
    var lim = s.list();
    var setLimit = function (pct) {
        return function () {
            power.limit(pct);
            batteryPage();
        };
    };
    var cur = b.limit;
    lim.add((cur === 100 ? "* " : "  ") + "100% まで充電", setLimit(100));
    lim.add((cur === 90 ? "* " : "  ") + "90% で停止", setLimit(90));
    lim.add((cur === 80 ? "* " : "  ") + "80% で停止", setLimit(80));
    if (cur < 100) {
        s.button("今回だけ満充電する", function () {
            power.fullOnce();
            batteryPage();
        });
    }

    s.label("持ち出し前など、一時的に上限を外したいときに使います。");

    s.button("電源を切る", function () {
        clearBattTimer();
        var c = ui.screen("電源を切る");
        c.label("保存していない内容は失われます。");
        c.label("Tab5 は電源が入っているときだけ充電できます。");
        c.button("電源を切る", function () { power.off(); });
        c.button("やめる", batteryPage);
    });

    // Bring-up: the INA226's orientation on this board is undocumented, so
    // the sign is a setting until a device run pins it down.
    if (b.ma < 0 && b.state === "charging") {
        s.label("※ 充電中なのに電流が負です。符号を反転してください。");
    }
    s.button("電流の符号を反転 (動作確認用)", function () {
        power.sign(b.ma >= 0 ? -1 : 1);
        batteryPage();
    });

    s.button("戻る", function () {
        clearBattTimer();
        mainPage();
    });

    battTimer = setInterval(function () {
        var n = power.battery();
        head.setText(battLine(n));
        detail.setText(battDetail(n));
    }, 1000);
}

function mainPage() {
    clearTsTimer();
    clearBattTimer();
    unwind();
    var s = ui.screen("デバイス設定");
    s.label("ネットワーク");
    var list = s.list();
    var wifi = system.wifiStatus();
    var ts = system.tailscaleStatus();
    list.add("Wi-Fi    " + (wifi.configured ? wifi.ssid : "未設定"),
             wifiPage);
    list.add("Tailscale    " + (ts.configured ? tsShort(ts) : "未設定"),
             tailscalePage);
    list.add("QRで設定を読み込む", qrTestPage);
    s.label("電源");
    var plist = s.list();
    plist.add("バッテリー    " + battShort(), batteryPage);
    s.label("この画面は端末ファームウェアに組み込まれています。");
    s.button("アプリ一覧へ戻る", function () {
        clearTsTimer();
        clearBattTimer();
        sys.open("launcher");
    });
}

function qrTestPage() {
    var s = ui.screen("QR読み取り");
    var result = s.label("プロビジョニングQRを読み取り、内容を確認してから適用します。");
    s.label("QRは暗号化されていません。秘密値は確認画面では伏せて表示します。");
    s.button("QRコードを読み取る", function () {
        result.setText("読み取り中...");
        if (!camera.scanQr(function (text) {
            if (text === undefined) {
                result.setText("読み取れませんでした\n" + camera.status());
                return;
            }
            var payload;
            try {
                payload = parseProvisioning(text);
            } catch (e) {
                result.setText("QRの内容が不正です: " + e.message);
                return;
            }
            provisionConfirmPage(payload);
        }))
            result.setText("カメラを開始できませんでした");
    });
    s.button("キャンセル", function () {
        camera.cancel();
    });
    s.button("戻る", mainPage);
}

function provisionConfirmPage(p) {
    var s = ui.screen("QR設定の確認");
    s.label("内容を確認して適用してください。このQRは暗号化されていません。");
    if (p.device !== undefined)
        s.label("対象端末: " + p.device);
    if (p.id !== undefined)
        s.label("ID: " + p.id);
    if (p.exp !== undefined)
        s.label("有効期限: " + p.exp + expiryInfo(p).note);
    if (p.wifi !== undefined) {
        s.label("Wi-Fi SSID: " + p.wifi.ssid);
        s.label("Wi-Fi パスワード: " +
            (p.wifi.password ? "設定あり（非表示）" : "なし（オープン）"));
    }
    if (p.tailscale !== undefined)
        s.label("Tailscale Auth key: 設定あり（非表示）");
    s.button("この設定を適用", function () {
        provisionResultPage(applyProvisioning(p));
    });
    s.button("キャンセル", mainPage);
}

function applyProvisioning(p) {
    var msg = "";
    if (p.wifi !== undefined) {
        var okw = system.wifiSet(p.wifi.ssid, p.wifi.password);
        msg += "Wi-Fi (" + p.wifi.ssid + "): " +
            (okw ? "保存しました" : "保存できませんでした") + "\n";
        p.wifi.password = ""; // drop the secret reference after use
    }
    if (p.tailscale !== undefined) {
        var okt = system.tailscaleSet(p.tailscale.authKey);
        msg += "Tailscale: " +
            (okt ? "保存しました" : "保存できませんでした") + "\n";
        p.tailscale.authKey = "";
    }
    return msg + "\n適用しました。Tailscaleは自動で接続を開始します。";
}

function provisionResultPage(text) {
    var s = ui.screen("適用結果");
    s.label(text);
    s.button("デバイス設定へ戻る", mainPage);
}

function wifiPage() {
    var status = system.wifiStatus();
    var s = ui.screen("Wi-Fi");
    var message = s.label(status.configured
        ? "保存済み: " + status.ssid
        : "保存済み設定はありません");
    var ssid = s.field("SSID");
    var password = s.field("パスワード", { secret: true });
    s.button("保存", function () {
        if (!ssid.value()) {
            message.setText("SSIDを入力してください");
            return;
        }
        if (system.wifiSet(ssid.value(), password.value())) {
            message.setText("保存しました。接続処理は未実装です");
            password.setText("");
        } else {
            message.setText("保存できませんでした");
        }
    });
    if (status.configured) {
        s.button("保存済み設定を削除", function () {
            message.setText(system.wifiForget()
                ? "削除しました"
                : "削除できませんでした");
        });
    }
    s.button("戻る", mainPage);
}

function tailscalePage() {
    clearTsTimer();
    var s = ui.screen("Tailscale");
    var st = system.tailscaleStatus();
    var status = s.label(tsStatusLine(st));

    if (st.configured) {
        s.button(st.enabled ? "オフにする" : "オンにする", function () {
            clearTsTimer();
            if (st.enabled) system.tailscaleDisable();
            else system.tailscaleEnable();
            tailscalePage();
        });
    }

    var authKey = s.field(st.configured ? "Auth key（差し替え）" : "Auth key",
                          { secret: true });
    s.button("保存", function () {
        if (!authKey.value()) {
            status.setText("Auth keyを入力してください");
            return;
        }
        if (system.tailscaleSet(authKey.value())) {
            authKey.setText("");
            clearTsTimer();
            tailscalePage();   // now configured + connecting
        } else {
            status.setText("保存できませんでした（長さを確認）");
        }
    });

    if (st.configured) {
        s.button("保存済みAuth keyを削除", function () {
            clearTsTimer();
            system.tailscaleForget();
            tailscalePage();
        });
    }
    s.button("戻る", function () {
        clearTsTimer();
        mainPage();
    });

    // live status: shows "接続中… 試行N回" -> "接続済み 100.x.y.z / peers M"
    tsTimer = setInterval(function () {
        status.setText(tsStatusLine(system.tailscaleStatus()));
    }, 2000);
}

sys.onForeground(mainPage);
mainPage();
