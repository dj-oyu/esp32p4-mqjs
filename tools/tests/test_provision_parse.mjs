// Host check for the device_settings.js MQJSP1 parser/validator.
// Loads the embedded app with UI/system stubs and exercises parseProvisioning
// against wire text emitted by mqjs_provision_qr.py.
import fs from "node:fs";

const src = fs.readFileSync(new URL("../../examples/device_settings.js", import.meta.url), "utf8");

// Chainable no-op UI so the file's top-level mainPage() render is harmless.
const handle = { setText() {}, value: () => "", add() {} };
const screen = {
    label: () => handle, list: () => handle, button() {}, field: () => handle,
};
const ui = { screen: () => screen, back: () => false };
const sys = { setAppName() {}, onForeground() {}, open() {} };
const captured = [];
const system = {
    wifiStatus: () => ({ configured: false }),
    tailscaleStatus: () => ({ configured: false }),
    wifiSet: (ssid, password) => { captured.push(["wifi", ssid, password]); return true; },
    tailscaleSet: (k) => { captured.push(["ts", k]); return true; },
    wifiForget: () => true, tailscaleForget: () => true,
};
const camera = { scanQr: () => true, cancel() {}, status: () => "" };

// Expose the file's function declarations by appending an export hook.
const factory = new Function(
    "sys", "ui", "system", "camera", "Date",
    src + "\nreturn { parseProvisioning, applyProvisioning, expiryInfo };"
);
const app = factory(sys, ui, system, camera, Date);

let pass = 0, fail = 0;
function ok(name, cond) {
    if (cond) { pass++; } else { fail++; console.log("FAIL:", name); }
}
// Wire text emitted by mqjs_provision_qr.py create (see harness comment).
const VECTORS = {
    prov_full: "MQJSP1:eyJkZXZpY2UiOiJ0YWI1LWExYjIiLCJpZCI6InNldHVwLTAwMSIsInRhaWxzY2FsZSI6eyJhdXRoS2V5IjoidHNrZXktYXV0aC14eXoxMjMifSwidiI6MSwid2lmaSI6eyJwYXNzd29yZCI6InBAc3Mg44Ov44O844OJISIsInNzaWQiOiLlrrbjga5XaUZpIn19",
    prov_wifi_open: "MQJSP1:eyJpZCI6IjZmZWFjZDgwLTMwZTAtNDlhNC04MGI3LTM5YmZmZTM1MmQ1NSIsInYiOjEsIndpZmkiOnsicGFzc3dvcmQiOiIiLCJzc2lkIjoib3BlbmFwIn19",
    prov_ts: "MQJSP1:eyJpZCI6IjUwMWYzNGQyLTJkMTQtNDYyNi1hNWQ0LWFkZDA1OTQ0NzM2OCIsInRhaWxzY2FsZSI6eyJhdXRoS2V5IjoidHNrZXktb25seSJ9LCJ2IjoxfQ",
    // exp ~2026-06-14; replaced below with a live far-future exp for the clock test
    prov_exp: "MQJSP1:eyJleHAiOjE3ODE0OTAxMTUsImlkIjoiMDdlNzgwMTAtMjY1My00MjFlLWE0MmMtMTUwZDIzMTI1OWRjIiwidiI6MSwid2lmaSI6eyJwYXNzd29yZCI6IiIsInNzaWQiOiJleHAifX0",
};
function read(f) { return VECTORS[f]; }

// --- valid vectors round-trip ---
const full = app.parseProvisioning(read("prov_full"));
ok("full v", full.v === 1);
ok("full device", full.device === "tab5-a1b2");
ok("full ssid utf8", full.wifi.ssid === "家のWiFi");
ok("full password utf8", full.wifi.password === "p@ss ワード!");
ok("full authKey", full.tailscale.authKey === "tskey-auth-xyz123");

const openw = app.parseProvisioning(read("prov_wifi_open"));
ok("open ssid", openw.wifi.ssid === "openap");
ok("open password empty", openw.wifi.password === "");
ok("open no tailscale", openw.tailscale === undefined);

const ts = app.parseProvisioning(read("prov_ts"));
ok("ts only authKey", ts.tailscale.authKey === "tskey-only");
ok("ts no wifi", ts.wifi === undefined);

const exp = app.parseProvisioning(read("prov_exp"));
ok("exp parsed", typeof exp.exp === "number" && exp.exp > 0);
// live clock-relative expiry (host clock is real / synced)
const nowSec = Math.floor(Date.now() / 1000);
function expVec(sec) {
    return "MQJSP1:" + Buffer.from(JSON.stringify(
        { v: 1, exp: sec, wifi: { ssid: "e", password: "" } })).toString("base64url");
}
ok("future exp not expired",
    app.expiryInfo(app.parseProvisioning(expVec(nowSec + 3600))).expired === false);
ok("past exp expired",
    app.expiryInfo(app.parseProvisioning(expVec(nowSec - 3600))).expired === true);

// --- apply forwards decoded values to system.* ---
captured.length = 0;
app.applyProvisioning(app.parseProvisioning(read("prov_full")));
ok("apply wifi forwarded",
    captured.some(c => c[0] === "wifi" && c[1] === "家のWiFi" && c[2] === "p@ss ワード!"));
ok("apply ts forwarded",
    captured.some(c => c[0] === "ts" && c[1] === "tskey-auth-xyz123"));

// --- rejection cases ---
function rejects(name, text) {
    try { app.parseProvisioning(text); ok(name, false); }
    catch (e) { ok(name, true); }
}
rejects("no prefix", "hello");
rejects("empty body", "MQJSP1:");
rejects("bad base64", "MQJSP1:****");
rejects("not json object", "MQJSP1:" + Buffer.from("123").toString("base64url"));
rejects("wrong version", "MQJSP1:" + Buffer.from(JSON.stringify({ v: 2, wifi: { ssid: "a", password: "" } })).toString("base64url"));
rejects("unknown field", "MQJSP1:" + Buffer.from(JSON.stringify({ v: 1, evil: 1, wifi: { ssid: "a", password: "" } })).toString("base64url"));
rejects("neither wifi nor ts", "MQJSP1:" + Buffer.from(JSON.stringify({ v: 1 })).toString("base64url"));
rejects("wifi extra key", "MQJSP1:" + Buffer.from(JSON.stringify({ v: 1, wifi: { ssid: "a", password: "", x: 1 } })).toString("base64url"));
rejects("ssid too long", "MQJSP1:" + Buffer.from(JSON.stringify({ v: 1, wifi: { ssid: "x".repeat(33), password: "" } })).toString("base64url"));
rejects("ssid empty", "MQJSP1:" + Buffer.from(JSON.stringify({ v: 1, wifi: { ssid: "", password: "" } })).toString("base64url"));
rejects("password too long", "MQJSP1:" + Buffer.from(JSON.stringify({ v: 1, wifi: { ssid: "a", password: "x".repeat(65) } })).toString("base64url"));
rejects("tailscale extra key", "MQJSP1:" + Buffer.from(JSON.stringify({ v: 1, tailscale: { authKey: "k", x: 1 } })).toString("base64url"));

// --- error messages never leak secret values ---
let leaked = false;
try { app.parseProvisioning("MQJSP1:" + Buffer.from(JSON.stringify({ v: 1, wifi: { ssid: "ok", password: "SUPERSECRET", x: 1 } })).toString("base64url")); }
catch (e) { leaked = e.message.indexOf("SUPERSECRET") >= 0; }
ok("error hides secret", leaked === false);

console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
