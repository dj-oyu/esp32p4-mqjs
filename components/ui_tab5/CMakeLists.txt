# Tab5 on-device UI. Sources are only compiled with CONFIG_MQJS_TAB5_UI=y;
# REQUIRES must stay unconditional (requirement expansion runs before
# kconfig), so Stamp builds still fetch+compile LVGL but link none of it —
# flash size is unaffected.
if(CONFIG_MQJS_TAB5_UI)
    set(UI_SRCS "ui_tab5.cpp" "fonts/font_noto_jp_20_4.c"
        # ST712x panel drivers vendored from M5Tab5-UserDemo (Apache-2.0)
        "vendor/esp_lcd_st7123.c" "vendor/esp_lcd_st7121.c")
else()
    set(UI_SRCS "")
endif()

idf_component_register(SRCS ${UI_SRCS}
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "vendor"
                       PRIV_REQUIRES esp_lcd esp_driver_i2c esp_driver_gpio
                                     esp_driver_ledc)

if(CONFIG_MQJS_TAB5_UI)
    # the lv_font_conv-generated font selects #include "lvgl.h" via this guard
    target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)
    # defer esp-hosted's constructor-time transport init (see ui_tab5.cpp)
    target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_hosted_init")
endif()
