// dev-slot probe: exercise the audio playback path end to end without
// serial. Reports audio.stats() across a tone and a WAV playback on
// <base>/proberep (frames climbing = I2S is consuming PCM), then stops.
// Restore dev_idle.js afterwards.
"use strict";
sys.setAppName("probe_audio");

var BASE = "esp32p4-mqjs/task/u7q3x9f2";

function pub(o) { mqtt.publish(BASE + "/proberep", JSON.stringify(o), 0, 0); }

function selfStop() {
    var a = sys.apps();
    for (var i = 0; i < a.length; i++)
        if (a[i].name === "probe_audio") sys.stop(a[i].slot);
}

mqtt.onConnect(function () {
    pub({ phase: "boot", stats: audio.stats() });

    audio.start(48000, 2);
    audio.volume(70);
    var toneOk = audio.tone(880, 300);
    pub({ phase: "tone", ok: toneOk, stats: audio.stats() });

    setTimeout(function () {
        pub({ phase: "after-tone", stats: audio.stats() });
        var wavOk = audio.playWav();
        pub({ phase: "playWav", ok: wavOk, stats: audio.stats() });
    }, 800);

    /* sample frames_written while the 6.3 s WAV streams: it must climb */
    setTimeout(function () { pub({ phase: "wav-2s", stats: audio.stats() }); }, 3000);
    setTimeout(function () { pub({ phase: "wav-5s", stats: audio.stats() }); }, 6000);
    setTimeout(function () {
        pub({ phase: "end", stats: audio.stats() });
        setTimeout(selfStop, 500);
    }, 9000);
});

net.onReady(function (token) { mqtt.connect(token); });
