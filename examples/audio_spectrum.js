// 16-band FFT analyzer + sound test + live high-pass A/B. Smooth render
// (chrome once, only changed bars repaint). Buttons: tone sweep, boot WAV,
// and an HPF preset cycler (off / fc / Q with resonance) applied live via
// audio.start(48000,2,fcHz,Qx10) — no new binding needed. The analyzer
// shows the POST-filter spectrum so the low cut + resonance bump are visible.
"use strict";
sys.setAppName("sndtest");

var WH = ui.size(), W = WH[0], H = WH[1];
var BG = 0x0B0E11, SUB = 0x6B7B8A, GRID = 0x202833;
var NB = 16, MX = 20, MR = 20, GAP = 6;
var BTN_H = 96, BTN_Y = H - 130;
var BASE = BTN_Y - 36, TOP = 150, MAXH = BASE - TOP;
var BW = ((W - MX - MR) / NB) | 0;
var B1 = { x: 16, w: 216, c: 0x2E6BD6, t: "トーン掃引" };
var B2 = { x: 252, w: 216, c: 0x8A4FE6, t: "WAV 再生" };
var B3 = { x: 488, w: 216, c: 0x3A7A4A };

// [fcHz, Q*10, label]   fcHz 0 = HPF off
var HPF = [[0, 7, "HPF off"], [120, 7, "120/.7"], [160, 10, "160/1.0"],
           [200, 14, "200/1.4"], [250, 20, "250/2.0"]];
var hi = 0;

var spec = [], mode = "READY", pk = 0, cl = 0, pc = 0;
var lastH = [], chromeOK = false, lastMode = null;

function colorFor(v) { return v > 75 ? 0xE65A57 : v > 45 ? 0xE6C84F : 0x2ED673; }
function btn(b, label) {
  ui.rect(b.x, BTN_Y, b.w, BTN_H, b.c);
  ui.text(b.x + 20, BTN_Y + 36, label, 0xFFFFFF);
}
function drawChrome() {
  ui.clear(BG);
  ui.rect(MX, BASE, W - MX - MR, 2, GRID);
  btn(B1, B1.t); btn(B2, B2.t); btn(B3, HPF[hi][2]);
  chromeOK = true; lastMode = null; lastH = [];
}
function render() {
  if (!chromeOK) drawChrome();
  if (mode !== lastMode) {
    ui.rect(0, 22, W, 44, BG);
    ui.text(24, 46, "16-band FFT  [" + mode + "]  pk=" + pk + " clip=" + cl, SUB);
    lastMode = mode;
  }
  for (var b = 0; b < NB; b++) {
    var v = spec[b] || 0;
    var h = (v * MAXH / 100) | 0; if (h < 3) h = 3;
    if (lastH[b] === h) continue;
    var x = MX + b * BW, cw = BW - GAP;
    ui.rect(x, TOP, cw, MAXH - h, BG);
    ui.rect(x, BASE - h, cw, h, colorFor(v));
    lastH[b] = h;
  }
}
function poll() {
  try {
    var o = JSON.parse(audio.stats());
    if (o && o.spec) { spec = o.spec; pk = o.peak; cl = o.clip; }
  } catch (e) {}
  render();
  if ((pc++ % 4) === 0)
    print("spec[" + mode + " " + HPF[hi][2] + "] pk=" + pk + " clip=" + cl + " | " + spec.join(" "));
}

audio.start(48000, 2);
audio.volume(70);

function applyHpf() {
  var p = HPF[hi];
  audio.hpf(p[0], p[1]);  // fcHz, Q*10 (0 = off) — proper binding
  btn(B3, p[2]);          // repaint just this button
  lastMode = null;        // force title refresh
}

var sweep = [120, 220, 440, 880], si = 0;
function sweepStep() {
  if (si >= sweep.length) { mode = "READY"; setTimeout(playWav, 800); return; }
  var f = sweep[si++];
  mode = f + "Hz";
  audio.tone(f, 500);
  setTimeout(sweepStep, 650);
}
function playSweep() { si = 0; mode = "sweep"; sweepStep(); }
function playWav() { mode = "WAV"; audio.playWav(); }

ui.onTouch(function (x, y, kind) {
  if (kind !== 0) return;
  if (y < BTN_Y || y > BTN_Y + BTN_H) return;
  if (x >= B1.x && x <= B1.x + B1.w) playSweep();
  else if (x >= B2.x && x <= B2.x + B2.w) playWav();
  else if (x >= B3.x && x <= B3.x + B3.w) { hi = (hi + 1) % HPF.length; applyHpf(); }
});
sys.onForeground(function () { chromeOK = false; render(); });
setTimeout(function () { sys.focus("sndtest"); }, 200);
setTimeout(playSweep, 1200);

setInterval(poll, 70);
print("sndtest spectrum + HPF A/B: tap 掃引 / WAV / HPF(切替)");
