# fs_picker: ネイティブのモーダル・ファイルピッカー / 同意ダイアログ
# (docs/native-editor-spec.md §A.4)。
#
# ソースは**常に**登録する。CONFIG_MQJS_TAB5_UI=n のときは fs_picker.cpp の
# 中身がまるごと #if で落ち、fs_pick_begin() が false を返す短いスタブだけが
# 残る。ui_tab5 のように SRCS を空にすると、このターゲットが INTERFACE
# ライブラリになって target_include_directories(... PRIVATE ...) が使えなく
# なる (ルート CMakeLists.txt の sshc に関する注記と同じ罠)。
idf_component_register(SRCS "fs_picker.cpp"
                       INCLUDE_DIRS "include"
                       # 公開ヘッダが fs_core.h (FS_PATH_MAX) を引くので
                       # fs_core は REQUIRES 側。
                       REQUIRES fs_core
                       PRIV_REQUIRES ui_tab5 esp_timer
                                     lvgl__lvgl espressif__esp_lvgl_port)

# ui_tab5 の非公開ヘッダ (ui_tab5_internal.h) を 1 本だけ引く。要るのは
# 日本語グリフを持つ ui_tab5_jp_font() で、これが無いとダイアログのラベルが
# すべて豆腐になる。宣言をこちらへ写す手 (mqjs_power.c が
# ui_tab5_screen_scrim にやっている手) を取らなかったのは、ui_tab5.cpp が
# このヘッダを extern "C" の外で include しているため — 関数は C++ の
# 名前修飾を持っており、写した宣言がずれるとリンク時にしか分からない。
if(CONFIG_MQJS_TAB5_UI)
    target_include_directories(${COMPONENT_LIB} PRIVATE
                               "${CMAKE_CURRENT_LIST_DIR}/../ui_tab5")
endif()
