// store catalog E2E probe (dev push, no @app: must NOT install itself)
"use strict";
var st = sys.store();
print("STORE rows=" + st.length);
for (var i = 0; i < st.length; i++)
    print("  " + st[i].name + " | " + st[i].title + " | " + st[i].size +
          "B | installed=" + st[i].installed + (st[i].perm ? " | perm=" + st[i].perm : ""));
print("INSTALL life -> " + sys.install("life"));
setTimeout(function () {
    var inst = sys.installed(), got = false;
    for (var i = 0; i < inst.length; i++)
        if (inst[i].name === "life") got = true;
    print("AFTER3S life installed=" + got);
    var st2 = sys.store();
    for (var j = 0; j < st2.length; j++)
        if (st2[j].name === "life") print("  catalog life installed=" + st2[j].installed);
    print("UNINSTALL life -> " + sys.uninstall("life"));
    var st3 = sys.store(), back = false;
    for (var k = 0; k < st3.length; k++)
        if (st3[k].name === "life" && !st3[k].installed) back = true;
    print("AFTER-UNINSTALL life back-in-catalog=" + back);
    print("E2E-DONE");
}, 3000);
