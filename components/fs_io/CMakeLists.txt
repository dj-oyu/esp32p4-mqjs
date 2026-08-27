# device only: このコンポーネントは flash / SDMMC に触る唯一の逃がし先。
idf_component_register(SRCS "fs_io.c"
                       INCLUDE_DIRS "include"
                       REQUIRES fs_core
                       PRIV_REQUIRES mqjs esp_timer)
