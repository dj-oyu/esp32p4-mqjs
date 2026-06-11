# The JS task to embed in the firmware. Pick any file from examples/:
#   idf.py -DMQJS_SCRIPT=life.js build flash
set(MQJS_SCRIPT "blink_button.js" CACHE STRING "Script in examples/ embedded as the JS task")

set(MQJS_SCRIPT_PATH ${CMAKE_CURRENT_LIST_DIR}/../examples/${MQJS_SCRIPT})
if(NOT EXISTS ${MQJS_SCRIPT_PATH})
    message(FATAL_ERROR "MQJS_SCRIPT '${MQJS_SCRIPT}' not found in examples/")
endif()

# Second embedded app for the P4a multi-app runtime (slot 2). An empty
# value embeds an empty file = no second app.
set(MQJS_APP2 "p4_bg_app.js" CACHE STRING "Script in examples/ embedded as the slot-2 app (empty = none)")

if(MQJS_APP2 STREQUAL "")
    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/app2.js "")
else()
    set(MQJS_APP2_PATH ${CMAKE_CURRENT_LIST_DIR}/../examples/${MQJS_APP2})
    if(NOT EXISTS ${MQJS_APP2_PATH})
        message(FATAL_ERROR "MQJS_APP2 '${MQJS_APP2}' not found in examples/")
    endif()
    configure_file(${MQJS_APP2_PATH} ${CMAKE_CURRENT_BINARY_DIR}/app2.js COPYONLY)
endif()

# The resident launcher app (P4b, slot 0).
configure_file(${CMAKE_CURRENT_LIST_DIR}/../examples/launcher.js
               ${CMAKE_CURRENT_BINARY_DIR}/launcher.js COPYONLY)

# Copy to a fixed name so the embedded symbol (_binary_task_js_*) does not
# depend on which script was selected. configure_file re-runs cmake when
# the source script changes.
configure_file(${MQJS_SCRIPT_PATH} ${CMAKE_CURRENT_BINARY_DIR}/task.js COPYONLY)

idf_component_register(SRCS "app_main.c" "wifi.c" "task_source.c" "storage.c"
                            "board_tab5.c" "ui_status.c"
                       PRIV_REQUIRES mqjs nvs_flash esp_wifi esp_netif esp_event
                                     tweetnacl esp_driver_i2c ui_tab5
                       EMBED_TXTFILES ${CMAKE_CURRENT_BINARY_DIR}/task.js
                                      ${CMAKE_CURRENT_BINARY_DIR}/app2.js
                                      ${CMAKE_CURRENT_BINARY_DIR}/launcher.js)

# the slot-2 app registers under its file name (= its sys.signal /
# sys.launch identity)
get_filename_component(MQJS_APP2_NAME_WE ${MQJS_APP2} NAME_WE)
target_compile_definitions(${COMPONENT_LIB} PRIVATE
                           MQJS_APP2_NAME="${MQJS_APP2_NAME_WE}")
