# mqjs component: MicroQuickJS engine + Stamp-P4 device runtime
#
# Build flow:
#   1. device_stdlib.c + mquickjs_build.c are compiled with the HOST
#      compiler into a generator tool (runs on the build machine).
#   2. The tool emits mquickjs_atom.h / device_stdlib.h with -m32
#      (ESP32-P4 is 32-bit RISC-V).
#   3. The engine (mquickjs.c) and the runtime are cross-compiled
#      against those generated headers.

set(MQJS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/mquickjs)
set(GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/gen)
set(PREGEN_DIR ${CMAKE_CURRENT_SOURCE_DIR}/gen)
file(MAKE_DIRECTORY ${GEN_DIR})

idf_component_register(
    SRCS "device_stdlib.c" "mqjs_runtime.c"
         "app/mqjs_app_manager.c"
         "${MQJS_DIR}/mquickjs.c"
         "${MQJS_DIR}/cutils.c"
         "${MQJS_DIR}/dtoa.c"
         "${MQJS_DIR}/libm.c"
    INCLUDE_DIRS "."
    PRIV_INCLUDE_DIRS "${MQJS_DIR}" "${GEN_DIR}"
    PRIV_REQUIRES driver esp_driver_gpio esp_driver_i2c esp_timer nvs_flash
                  ui_tab5 sshc cam_tab5 esp_http_client esp-tls mbedtls
)

# Vendored engine: uint32_t is 'long unsigned int' on riscv32, so the
# upstream %d/%x format strings trip -Werror=format. -O2 additionally
# trips -Werror=dangling-pointer on the stack-scanned GC-root pattern
# (string_buffer_push stores &local into ctx->top_gc_ref on purpose;
# the root is popped before return). Keep upstream pristine and relax
# the warnings for those files only.
set_source_files_properties(
    ${MQJS_DIR}/mquickjs.c ${MQJS_DIR}/cutils.c ${MQJS_DIR}/dtoa.c ${MQJS_DIR}/libm.c
    PROPERTIES COMPILE_OPTIONS "-Wno-format;-Wno-type-limits;-Wno-dangling-pointer"
)

# device_stdlib.c is primarily the HOST generator source (its main() is
# discarded by --gc-sections in the firmware link); it is in SRCS so the
# IDE/clangd indexes it. The build-mode macros leave fields uninitialized.
set_source_files_properties(
    device_stdlib.c
    PROPERTIES COMPILE_OPTIONS "-Wno-missing-field-initializers"
)

if(CMAKE_HOST_WIN32)
    # Windows toolchains in ESP-IDF can emit ELF host tools that are not runnable on Windows.
    # Use pre-generated headers committed in the component for reliable local builds.
    add_custom_command(
    OUTPUT ${GEN_DIR}/mquickjs_atom.h ${GEN_DIR}/device_stdlib.h
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${PREGEN_DIR}/mquickjs_atom.h ${GEN_DIR}/mquickjs_atom.h
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${PREGEN_DIR}/device_stdlib.h ${GEN_DIR}/device_stdlib.h
    DEPENDS ${PREGEN_DIR}/mquickjs_atom.h ${PREGEN_DIR}/device_stdlib.h
    COMMENT "Using pre-generated ROM stdlib headers on Windows"
    VERBATIM
    )
else()
    # Host-side generator tool (NOT cross-compiled)
    find_program(HOST_CC NAMES cc gcc clang REQUIRED)

    add_custom_command(
    OUTPUT ${GEN_DIR}/stdlib_tool
    COMMAND ${HOST_CC} -O2 -I${MQJS_DIR}
        -o ${GEN_DIR}/stdlib_tool
        ${CMAKE_CURRENT_SOURCE_DIR}/device_stdlib.c
        ${MQJS_DIR}/mquickjs_build.c
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/device_stdlib.c
        ${MQJS_DIR}/mquickjs_build.c
    COMMENT "Building mquickjs stdlib host tool"
    VERBATIM
    )

    # ESP32-P4 = 32-bit RISC-V -> -m32.
    # (The 'Too many properties' warning from the tool is harmless: the
    #  global-object hash size is capped, costing a few lookup cycles.)
    add_custom_command(
    OUTPUT ${GEN_DIR}/mquickjs_atom.h ${GEN_DIR}/device_stdlib.h
    COMMAND ${CMAKE_COMMAND}
        -DSTD_TOOL=${GEN_DIR}/stdlib_tool
        -DATOM_HEADER=${GEN_DIR}/mquickjs_atom.h
        -DSTDLIB_HEADER=${GEN_DIR}/device_stdlib.h
        -P ${CMAKE_CURRENT_LIST_DIR}/gen_stdlib_headers.cmake
    DEPENDS ${GEN_DIR}/stdlib_tool
    COMMENT "Generating ROM stdlib headers (-m32)"
    VERBATIM
    )
endif()

add_custom_target(mqjs_stdlib_gen
    DEPENDS ${GEN_DIR}/mquickjs_atom.h ${GEN_DIR}/device_stdlib.h)
add_dependencies(${COMPONENT_LIB} mqjs_stdlib_gen)
