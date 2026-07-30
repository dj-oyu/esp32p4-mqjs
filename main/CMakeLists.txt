# The JS task to embed in the firmware. Pick any file from examples/:
#   idf.py -DMQJS_SCRIPT=life.js build flash
set(MQJS_SCRIPT "blink_button.js" CACHE STRING "Script in examples/ embedded as the JS task")

set(MQJS_SCRIPT_PATH ${CMAKE_CURRENT_LIST_DIR}/../examples/${MQJS_SCRIPT})
if(NOT EXISTS ${MQJS_SCRIPT_PATH})
    message(FATAL_ERROR "MQJS_SCRIPT '${MQJS_SCRIPT}' not found in examples/")
endif()

# The resident launcher (P4b, slot 0) is the ONLY embedded app; everything
# else is installed over MQTT ("// @app <name>" push -> /littlefs/apps/)
# and started from the launcher.
configure_file(${CMAKE_CURRENT_LIST_DIR}/../examples/launcher.js
               ${CMAKE_CURRENT_BINARY_DIR}/launcher.js COPYONLY)
configure_file(${CMAKE_CURRENT_LIST_DIR}/../examples/device_settings.js
               ${CMAKE_CURRENT_BINARY_DIR}/device_settings.js COPYONLY)

# Copy to a fixed name so the embedded symbol (_binary_task_js_*) does not
# depend on which script was selected. configure_file re-runs cmake when
# the source script changes.
configure_file(${MQJS_SCRIPT_PATH} ${CMAKE_CURRENT_BINARY_DIR}/task.js COPYONLY)

idf_component_register(SRCS "app_main.c" "wifi.c" "task_source.c" "storage.c"
                            "board_tab5.c" "ui_status.c" "ppa_bench.c"
                            "jsmem_bench.c" "panic_note.c"
                       PRIV_REQUIRES mqjs term_core nvs_flash esp_wifi esp_netif esp_event
                                     tweetnacl esp_driver_i2c ui_tab5 cam_tab5
                                     audio_tab5 audio_device kbd_tab5
                                     esp_driver_ppa
                                     esp_timer esp_opus_float opus_p4_kernels
                                     opus_player
                       EMBED_TXTFILES ${CMAKE_CURRENT_BINARY_DIR}/task.js
                                      ${CMAKE_CURRENT_BINARY_DIR}/launcher.js
                                      ${CMAKE_CURRENT_BINARY_DIR}/device_settings.js)

# panic_note.c: IDF 6 offers no hook on the panic path, so the black box's
# "panic 理由" line (§4.4) is written from a wrap of esp_panic_handler. Same
# mechanism components/ui_tab5 uses for esp_hosted_init / lvgl_port_ppa_create.
target_link_options(${COMPONENT_LIB} INTERFACE
                    "-Wl,--wrap=esp_panic_handler")
