// widget_leak_probe.js — W1-4 isolation probe.
// settings_demo.js prints one console line per cycle, and every console
// line allocates a retained LVGL label (by design, 200-line ring) — that
// alone shows up as a steady lvgl-pool decline. This probe runs the same
// screen churn but prints ONLY at start/end, so any remaining delta is a
// real widget leak. It also forces extra JS garbage so the compacting GC
// runs and frees JsUiHandle opaques (internal-RAM noise in the demo).

sys.setAppName("widget_leak_probe");
function page(n) {
  var s = ui.screen("Probe " + n);
  s.label("leak probe");
  var f = s.field("Host");
  s.toggle("WiFi", 1, function (v) {});
  s.slider(0, 100, 50, function (v) {});
  var l = s.list();
  for (var i = 0; i < 6; i++) l.add("item " + i, function () {});
  s.button("OK", ui.back);
  s.button("NG", ui.back);
}

var h0 = sys.heap();
var n = 0, pushing = true, junk = null;

var t = setInterval(function () {
  if (pushing) {
    page(++n);
    if (n % 5 === 0) pushing = false;
  } else if (!ui.back()) {
    pushing = true;
    junk = new Array(200).join("x" + n); // GC pressure
    if (n >= 100) { // 20 supercycles, zero prints in between
      clearInterval(t);
      var h1 = sys.heap();
      print("probe done after " + n + " screens:");
      print("  int   " + h0[0] + " -> " + h1[0] + " (d=" + (h1[0] - h0[0]) + ")");
      print("  psram " + h0[1] + " -> " + h1[1] + " (d=" + (h1[1] - h0[1]) + ")");
      print("  lvgl  " + h0[2] + " -> " + h1[2] + " (d=" + (h1[2] - h0[2]) + ")");
    }
  }
}, 300);
