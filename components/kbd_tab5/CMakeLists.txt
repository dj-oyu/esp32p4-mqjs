# M5Stack Tab5 Keyboard (A164, 70-key dock, STM32F030 @ I2C 0x6D).
# Same conditional-sources pattern as audio_tab5/ui_tab5: REQUIRES stay
# unconditional, sources only compile when the feature is on.
if(CONFIG_MQJS_TAB5_KEYBOARD)
    set(KBD_SRCS "kbd_tab5.c")
else()
    set(KBD_SRCS "")
endif()

idf_component_register(SRCS ${KBD_SRCS}
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_driver_i2c esp_driver_gpio esp_timer
                                     kbd_core mqjs)
