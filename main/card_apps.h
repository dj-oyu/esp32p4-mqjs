#pragma once

/* microSD 上のアプリを、棚 (MQTT) と同じカタログ契約でランチャーの
   ストア画面へ供給する (docs/filer-storage-design.md §13)。
   走査はストア画面を開いたときにだけ走るので、常に呼んでよい。
   SD ボリュームが無いボードでは黙って「0 本」になる。

   一覧は署名を確かめない (1 本 226ms かかり、JS タスクを止める)。
   署名が合わないファイルも行としては出てきて、crypto_sign_open は
   インストールのときに走り、落ちれば入らない。 */
void card_apps_init(void);
