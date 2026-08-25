# Tab5 battery: INA226 monitoring + charge control + fuel gauge + protection.
#
# Sources are only compiled with CONFIG_MQJS_TAB5_BATTERY=y; REQUIRES must
# stay unconditional (requirement expansion runs before kconfig), so Stamp
# builds still resolve ui_tab5 but link none of this — same pattern as
# audio_tab5 and ui_tab5.
if(CONFIG_MQJS_TAB5_BATTERY)
    set(PWR_SRCS "pwr_tab5.c" "pwr_gauge.c")
else()
    set(PWR_SRCS "")
endif()

idf_component_register(SRCS ${PWR_SRCS}
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_driver_i2c esp_timer nvs_flash ui_tab5)
