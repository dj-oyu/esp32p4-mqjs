# hoststub — fs_picker.cpp をホストの g++ に通すための最小ヘッダ

`run_tests.sh` の 2 本目のスイート (`syntax`) 用。ESP-IDF のヘッダだけを
薄い偽物で置き換え、**LVGL・fs_core・fs_grant・ui_tab5 は本物を読ませる**。

なぜこれが要るのか: `fs_picker.cpp` は `idf.py build` でしか通らないので、
デバイスを持たない実装者が「lv_* の引数順を 1 つ取り違えた」ことに
気づける手段が無かった。ここでの `-fsyntax-only` は本物の LVGL 9.4 の
プロトタイプに突き合わせるので、引数の数・順・型と `LV_SYMBOL_*` /
`LV_STATE_*` / `LV_OPA_*` の綴りはここで落ちる。

**これで見ていないもの:**

- 実行時の挙動 (何も走らない。構文と型だけ)
- `lv_conf.h` は LVGL 同梱の **テンプレート** から生成する。デバイスの
  `sdkconfig.tab5` の設定とは別物なので、「デバイスの config で
  `LV_USE_KEYBOARD` が落ちている」といった食い違いはここでは出ない
  (2026-08-26 時点の `sdkconfig.tab5` では KEYBOARD/LIST/TEXTAREA/
  LABEL/BUTTON/FLEX はすべて y)
- IDF 側の関数 (`heap_caps_calloc` / `lvgl_port_lock` / `taskENTER_CRITICAL`
  / `xTaskGetCurrentTaskHandle` / `vTaskDelay` / `xTaskGetTickCount` など)
  は**このディレクトリの宣言**に照らして検査される。本物の IDF
  ヘッダではない。ここの宣言が本物とずれていたら、この検査は嘘をつく
  —— 直したときは本物と突き合わせること
  (2026-08-26 に足した FreeRTOS の 3 本は IDF 6.0.1 の
  `components/freertos/FreeRTOS-Kernel/include/freertos/task.h` と
  突き合わせ済み。`configTICK_RATE_HZ` は `sdkconfig.tab5` の 100 に
  合わせてある —— ここを 1000 にすると `pdMS_TO_TICKS(2)` が 0 に
  潰れなくなり、`fs_pick_cancel` の刻み幅のクランプが検査から消える)
- リンク (`-fsyntax-only`)。`ui_tab5_jp_font()` が実際に解決するかは
  `idf.py build` でしか分からない
