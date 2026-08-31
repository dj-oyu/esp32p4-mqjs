# microSD ドライバ。CONFIG_MQJS_SDCARD が無効なときも TU は残し、
# sdcard_init() が「登録しない」を返すだけの実装に切り替わる (sdcard.c)。
# 呼び出し側に #ifdef を書かせないための形で、実体が無ければ
# --gc-sections が丸ごと落とすので Stamp の flash は増えない
# (ui_tab5 / pwr_tab5 が LVGL に対してやっているのと同じ扱い)。
#
# REQUIRES は無条件でなければならない: 依存の展開は kconfig より先に走る。
idf_component_register(SRCS "sdcard.c" "sdmmc_share.c"
                       INCLUDE_DIRS "include"
                       REQUIRES fs_core
                       PRIV_REQUIRES esp_driver_sdmmc sdmmc fatfs vfs)

if(CONFIG_MQJS_SDCARD)
    # P4 の SDMMC コントローラは 1 個で、microSD (slot 0) と esp-hosted の
    # C6 リンク (slot 1) が両方 sdmmc_host_init() を呼ぶ。素のままだと
    # 2 人目が必ず落ちて Wi-Fi がブートループするので、参照カウントを
    # 噛ませて共有させる。理由の全量は sdmmc_share.c の先頭。
    target_link_options(${COMPONENT_LIB} INTERFACE
                        "-Wl,--wrap=sdmmc_host_init")
endif()
