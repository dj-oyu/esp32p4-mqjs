// settings_demo.js — W1-3/W1-4: widget-framework settings-page skeleton +
// heap-stability measurement (docs/widget-framework-design.md §4).
//
// Builds a full settings page (labels, fields incl. password, toggle,
// slider, host list, Save/Cancel), cycles push-5-deep/pop-to-console a few
// times logging sys.heap() (= [internal, psram, lvgl_pool]; the lvgl value
// must stay flat — the full 8-cycle quantitative run already passed on
// hardware 2026-06-11, see design §10.4), then leaves an interactive
// screen up. List taps / Save write into the status label on the screen
// itself, so the callback round-trip is visible without the console.

sys.setAppName("settings_demo");
function heapLine(tag) {
  var h = sys.heap();
  print(tag + " heap: int=" + h[0] + " psram=" + h[1] + " lvgl=" + h[2]);
}

function buildSettings(n) {
  var s = ui.screen("Settings #" + n);
  s.label("ウィジェットフレームワーク W1 デモ");
  var status = s.label("(リスト行をタップすると ここに出ます)");
  var host = s.field("Host");
  var user = s.field("User");
  var pass = s.field("Password", { secret: true });
  var wifi = s.toggle("WiFi", 1, function (v) { status.setText("WiFi -> " + v); });
  var bri = s.slider(0, 100, 70, function (v) { status.setText("brightness -> " + v); });
  var l = s.list();
  for (var i = 0; i < 6; i++) {
    (function (k) {
      l.add("host-" + k + ".local", function () {
        status.setText("tapped: host-" + k + ".local");
      });
    })(i);
  }
  s.button("Save", function () {
    status.setText("saved: host=" + host.value() + " user=" + user.value() +
                   " pass.len=" + pass.value().length +
                   " wifi=" + wifi.value() + " bri=" + bri.value());
  });
  s.button("Cancel", ui.back);
  return s;
}

heapLine("start");

var n = 0;
var cycles = 0;
var SUPERCYCLES = 2; // short: the 8-cycle heap gate already passed on hw
var pushing = true;

var t = setInterval(function () {
  if (pushing) {
    buildSettings(++n);
    if (n % 5 === 0) pushing = false;
  } else if (!ui.back()) {
    cycles++;
    heapLine("cycle " + cycles);
    if (cycles >= SUPERCYCLES) {
      clearInterval(t);
      heapLine("end");
      buildSettings(++n); // interactive screen for hands-on poking
      print("settings_demo done (" + n + " screens built)");
      return;
    }
    pushing = true;
  }
}, 500);
