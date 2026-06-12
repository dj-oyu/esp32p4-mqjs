# Tab5 camera barcode scanner. cam_tab5.c compiles to stubs unless
# CONFIG_MQJS_CAMERA=y (so Stamp/PC builds stay camera-free); ean13.c is
# pure C and host-testable (tools/ean13_test.c).
#
# esp_video (managed dep, idf_component.yml) is fetched for every build
# like LVGL is on the Stamp: its sources compile to ~nothing without the
# CSI/ISP kconfig options that sdkconfig.tab5.defaults enables.
if(CONFIG_MQJS_CAMERA)
    set(CAM_SRCS "cam_tab5.c" "ean13.c" "bc_locate.c")
    # PIE (xesppie) hand-fused structure-tensor kernel: P4-only. The .S
    # is fully guarded internally (CONFIG_IDF_TARGET_ESP32P4) so it is
    # harmless on other targets, but we only add it to the build on P4.
    if(CONFIG_IDF_TARGET_ESP32P4)
        list(APPEND CAM_SRCS "bc_tensor_p4.S")
    endif()
else()
    set(CAM_SRCS "cam_tab5.c")
endif()

idf_component_register(SRCS ${CAM_SRCS}
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_driver_ledc esp_driver_i2c esp_timer
                                     esp_driver_ppa ui_tab5)

# No per-file -march needed: the IDF P4 toolchain (GCC 15.2) already
# assembles the PIE + hardware-loop mnemonics. Its global asmflags carry
# -march=...xespv2p1_xesploop (PIE = xespv2p1, Xhwlp = xesploop) — the
# same flags main/ppa_bench.c's esp.vld/vst rely on. (NB: an older
# toolchain spelled the PIE extension "xesppie"; do NOT pin that name
# here — GCC 15.2 rejects it as an unknown non-standard extension and
# clobbering the global -march would also drop zaamo/zalrsc/xesploop.)
