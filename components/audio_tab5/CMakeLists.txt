# Tab5 speaker path: ES8388 (esp_codec_dev) + I2S TX + PCM ring buffer.
# Sources are only compiled with CONFIG_MQJS_TAB5_AUDIO=y; REQUIRES must
# stay unconditional (requirement expansion runs before kconfig), so
# Stamp builds still fetch esp_codec_dev but link none of it — same
# pattern as ui_tab5.
if(CONFIG_MQJS_TAB5_AUDIO)
    set(AUDIO_SRCS "audio_tab5.c")
else()
    set(AUDIO_SRCS "")
endif()

idf_component_register(SRCS ${AUDIO_SRCS}
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_driver_i2s esp_driver_i2c
                                     esp_driver_gpio esp_ringbuf ui_tab5)
