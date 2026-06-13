# audio_device: token-based arbitration over the single Tab5 speaker.
# Sources compile only with CONFIG_MQJS_TAB5_AUDIO=y; when off, audio_device.h
# provides no-op inline stubs (same pattern as audio_tab5 / ui_tab5). REQUIRES
# stays unconditional — requirement expansion runs before kconfig.
if(CONFIG_MQJS_TAB5_AUDIO)
    set(AUDEV_SRCS "audio_device.c" "audio_device_core.c"
                   "audio_device_selftest.c")
else()
    set(AUDEV_SRCS "")
endif()

idf_component_register(SRCS ${AUDEV_SRCS}
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES audio_tab5 esp_ringbuf)
