# edit_ui: edit_task とプレゼンタ (docs/native-editor-spec.md §A.2 / §A.3)。
#
# ソースは**常に**登録する。CONFIG_MQJS_TAB5_UI=n のときは edit_ui.c の中身が
# まるごと #if で落ち、edit_ui_start() が false を返す短いスタブだけが残る。
# SRCS を空にするとこのターゲットが INTERFACE ライブラリになり、
# target_* が使えなくなる (fs_picker の CMakeLists と同じ罠)。
#
# ---- REQUIRES が見張りになっている ----------------------------------
#
# **fs_core がここに無いのは意図的**。仕様 §A.2 は edit_task から
# fs_* / esp_partition_* / NVS を呼ぶことを禁じており、ファイル入出力は
# fs_io (§A.7、M2) へ投げる形になる。依存を書かないでおけば、うっかり
# 呼んだ瞬間にビルドが落ちる —— レビューより先に機械が気づく。
# **fs_* をここへ足したくなったら、それは fs_io へ回すべき仕事。**
# その fs_io は M2 で入った (PRIV_REQUIRES にある)。edit_task が触るのは
# fsio_submit だけで、実際の読み書きは fs_io タスク (core 0, prio 3) の上。
# fs_picker も §A の依存の向き (edit_ui -> fs_io, fs_picker) が許している ——
# あれは I/O ではなく UI モーダルで、cb は UI タスクの上で呼ばれる。
#
# 公開ヘッダ edit_ui.h が edit_core.h と skk_core.h を引くので、その 2 つは
# REQUIRES 側。ime_core は skk_core を REQUIRES しており、edit_ui.c だけが
# 使うので PRIV_REQUIRES。
idf_component_register(SRCS "edit_ui.c"
                       INCLUDE_DIRS "include"
                       REQUIRES edit_core skk_core
                       PRIV_REQUIRES ime_core ui_tab5 mqjs fs_io fs_picker
                                     esp_driver_ppa esp_timer esp_common
                                     freertos)
