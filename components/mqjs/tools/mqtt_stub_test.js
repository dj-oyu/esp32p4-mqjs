/* PC-only smoke test for the mqtt.* stubs (gpio-style print stubs) */
mqtt.connect("mqtt://test:1883");
mqtt.onConnect(function () { print("onConnect fired"); });
mqtt.subscribe("a/+/c", function (t, p) { print(t, p); });
print("connected =", mqtt.connected());
mqtt.publish("x/y", "hello", 1, 0);
mqtt.publish("x/y", "no-qos");
mqtt.disconnect();
print("mqtt stub test done");
