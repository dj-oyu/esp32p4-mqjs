# The JS task to embed in the firmware. Pick any file from examples/:
#   idf.py -DMQJS_SCRIPT=life.js build flash
set(MQJS_SCRIPT "blink_button.js" CACHE STRING "Script in examples/ embedded as the JS task")

set(MQJS_SCRIPT_PATH ${CMAKE_CURRENT_LIST_DIR}/../examples/${MQJS_SCRIPT})
if(NOT EXISTS ${MQJS_SCRIPT_PATH})
    message(FATAL_ERROR "MQJS_SCRIPT '${MQJS_SCRIPT}' not found in examples/")
endif()

# Copy to a fixed name so the embedded symbol (_binary_task_js_*) does not
# depend on which script was selected. configure_file re-runs cmake when
# the source script changes.
configure_file(${MQJS_SCRIPT_PATH} ${CMAKE_CURRENT_BINARY_DIR}/task.js COPYONLY)

idf_component_register(SRCS "app_main.c" "wifi.c"
                       PRIV_REQUIRES mqjs nvs_flash esp_wifi esp_netif esp_event
                       EMBED_TXTFILES ${CMAKE_CURRENT_BINARY_DIR}/task.js)
