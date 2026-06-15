idf_component_register(
    SRCS
        "src/wireguard.c"
        "src/wireguardif.c"
        "src/crypto.c"
        "src/wireguard-platform-esp32.c"
        "src/crypto/refc/blake2s.c"
        "src/crypto/refc/chacha20.c"
        "src/crypto/refc/chacha20poly1305.c"
        "src/crypto/refc/poly1305-donna.c"
        "src/crypto/refc/x25519.c"
    INCLUDE_DIRS
        "src"
        "src/crypto/refc"
    REQUIRES
        lwip
        esp_timer
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    WIREGUARD_CRYPTO_REFC=1
)

# GCC 15 (IDF 6.0) promotes -Wunterminated-string-initialization to an error.
# wireguard.c initialises fixed-size unsigned char arrays from string literals
# (Noise/WG construction identifiers) intentionally without room for the NUL.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error=unterminated-string-initialization)

# Disable stringop-overread warning for x25519.c (false positive in GCC array bounds check)
set_source_files_properties(
    src/crypto/refc/x25519.c
    PROPERTIES COMPILE_FLAGS "-Wno-stringop-overread"
)
