if(CONFIG_OPUS_PLAYER)
    set(OPUS_PLAYER_SRCS "ogg_opus.c" "opus_player.c")
else()
    set(OPUS_PLAYER_SRCS "")
endif()

set(OPUS_PLAYER_EMBED "")
if(CONFIG_OPUS_PLAYER_BOOT_OPUS)
    set(OPUS_PLAYER_EMBED "${CMAKE_SOURCE_DIR}/assets/audio/tab5-boot-48k.opus")
endif()

idf_component_register(SRCS ${OPUS_PLAYER_SRCS}
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "."
                       PRIV_REQUIRES audio_tab5 esp_opus_float heap
                       EMBED_FILES ${OPUS_PLAYER_EMBED})
