// dev-slot probe: exercise http.get end to end without serial — real
// HTTPS GET against NDL OpenSearch, report status/length/title-extract
// on <base>/proberep, then stop. Restore dev_idle.js afterwards.
"use strict";
sys.setAppName("probe_http");

var BASE = "esp32p4-mqjs/task/u7q3x9f2";

function pub(o) { mqtt.publish(BASE + "/proberep", JSON.stringify(o), 0, 0); }

function selfStop() {
    var a = sys.apps();
    for (var i = 0; i < a.length; i++)
        if (a[i].name === "probe_http") sys.stop(a[i].slot);
}

mqtt.onConnect(function () {
    if (typeof http === "undefined") {
        pub({ phase: "http", err: "no http binding" });
        setTimeout(selfStop, 1000);
        return;
    }
    var rc = http.get(
        "https://ndlsearch.ndl.go.jp/api/opensearch?isbn=9784101010014",
        function (body, st) {
            var m = body ? /<dc:title>([^<]*)<\/dc:title>/.exec(body) : null;
            var e = body ? /<dc:extent>([^<]*)<\/dc:extent>/.exec(body) : null;
            pub({ phase: "http-done", status: st,
                  len: body ? body.length : 0,
                  title: m ? m[1] : null,
                  extent: e ? e[1] : null });
            setTimeout(selfStop, 1000);
        });
    pub({ phase: "http-start", rc: rc });
    if (!rc)
        setTimeout(selfStop, 2000);
});
mqtt.connect("mqtt://192.168.1.2");
