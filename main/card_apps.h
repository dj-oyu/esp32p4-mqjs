#pragma once

/* microSD 上の署名済みアプリを、棚 (MQTT) と同じカタログ契約で
   ランチャーのストア画面へ供給する (docs/filer-storage-design.md §13)。
   走査はストア画面を開いたときにだけ走るので、常に呼んでよい。
   SD ボリュームが無いボードでは黙って「0 本」になる。 */
void card_apps_init(void);
