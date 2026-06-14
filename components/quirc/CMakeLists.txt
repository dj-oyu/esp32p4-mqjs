idf_component_register(
    SRCS "src/decode.c" "src/identify.c" "src/quirc.c" "src/version_db.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src")

# ESP32-P4 has hardware floating point; using float keeps identify.c smaller
# and faster than quirc's default double-precision geometry.
target_compile_definitions(${COMPONENT_LIB} PRIVATE QUIRC_FLOAT_TYPE=float QUIRC_USE_TGMATH)
