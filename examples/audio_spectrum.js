// 16-band FFT spectrum analyzer + sound test. Polls audio.stats().spec
// (computed on-device) ~14 Hz, draws 16 bars, and logs the band values so
// the spectrum can also be read over the serial monitor. Auto-plays a tone
// sweep on first foreground so a capture sees known signals; buttons replay
// the sweep or the boot WAV.
"use strict";
sys.setAppName("sndtest");

var WH = ui.size(), W = WH[0], H = WH[1];
var BG = 0x0B0E11, SUB = 0x6B7B8A, GRID = 0x202833;
var NB = 16, MX = 20, MR = 20, GAP = 6;
var BTN_H = 96, BTN_Y = H - 130;
var BASE = BTN_Y - 36, TOP = 150, MAXH = BASE - TOP;
var BW = ((W - MX - MR) / NB) | 0;
var B1 = { x: 20, w: 320, c: 0x2E6BD6, t: "トーン掃引" };
var B2 = { x: W - 340, w: 320, c: 0x8A4FE6, t: "WAV 再生" };

var spec = [], mode = "READY", pc = 0;

function colorFor(v) {
  if (v > 75) return 0xE65A57;
  if (v > 45) return 0xE6C84F;
  return 0x2ED673;
}
function draw() {
  ui.clear(BG);
  ui.text(24, 44, "16-band FFT  [" + mode + "]", SUB);
  ui.rect(MX, BASE, W - MX - MR, 2, GRID);
  for (var b = 0; b < NB; b++) {
    var v = spec[b] || 0;
    var h = (v * MAXH / 100) | 0;
    if (h < 2) h = 2;
    ui.rect(MX + b * BW, BASE - h, BW - GAP, h, colorFor(v));
  }
  ui.rect(B1.x, BTN_Y, B1.w, BTN_H, B1.c);
  ui.text(B1.x + 95, BTN_Y + 36, B1.t, 0xFFFFFF);
  ui.rect(B2.x, BTN_Y, B2.w, BTN_H, B2.c);
  ui.text(B2.x + 105, BTN_Y + 36, B2.t, 0xFFFFFF);
}
function poll() {
  try { var o = JSON.parse(audio.stats()); if (o && o.spec) spec = o.spec; } catch (e) {}
  draw();
  if ((pc++ % 7) === 0)   // ~ every 490 ms
    print("spec[" + mode + "] " + spec.join(" "));
}

audio.start(48000, 2);
audio.volume(70);

var sweep = [120, 220, 440, 880], si = 0;
function sweepStep() {
  if (si >= sweep.length) { mode = "READY"; return; }
  var f = sweep[si++];
  mode = f + "Hz";
  audio.tone(f, 500);
  setTimeout(sweepStep, 650);
}
function playSweep() { si = 0; mode = "sweep"; sweepStep(); }
function playWav() { mode = "WAV"; audio.playWav(); }

ui.onTouch(function (x, y, kind) {
  if (kind !== 0) return;
  if (y >= BTN_Y && y <= BTN_Y + BTN_H) {
    if (x >= B1.x && x <= B1.x + B1.w) playSweep();
    else if (x >= B2.x && x <= B2.x + B2.w) playWav();
  }
});

var firstFg = true;
sys.onForeground(function () {
  draw();
  if (firstFg) { firstFg = false; setTimeout(playSweep, 900); }
});
setTimeout(function () { sys.focus("sndtest"); }, 200);

setInterval(poll, 70);
draw();
print("sndtest spectrum+log: auto sweep on focus; tap トーン掃引 / WAV 再生");
