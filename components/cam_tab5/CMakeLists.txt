# Tab5 camera barcode scanner. cam_tab5.c compiles to stubs unless
# CONFIG_MQJS_CAMERA=y (so Stamp/PC builds stay camera-free); ean13.c is
# pure C and host-testable (tools/ean13_test.c).
#
# esp_video (managed dep, idf_component.yml) is fetched for every build
# like LVGL is on the Stamp: its sources compile to ~nothing without the
# CSI/ISP kconfig options that sdkconfig.tab5.defaults enables.
if(CONFIG_MQJS_CAMERA)
    set(CAM_SRCS "cam_tab5.c" "ean13.c")
else()
    set(CAM_SRCS "cam_tab5.c")
endif()

idf_component_register(SRCS ${CAM_SRCS}
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_driver_ledc esp_driver_i2c esp_timer)
