set(OPUS_P4_KERNEL_SRCS "opus_p4_kernels.c")

if(CONFIG_OPUS_P4_KERNEL_ASM OR CONFIG_OPUS_P4_KERNEL_ASM_VERIFY)
    list(APPEND OPUS_P4_KERNEL_SRCS "opus_p4_anti_collapse_noise_p4.S")
endif()

idf_component_register(SRCS ${OPUS_P4_KERNEL_SRCS}
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES heap)

target_compile_options(${COMPONENT_LIB} PRIVATE -O2)

# No per-file -march override: ESP-IDF's ESP32-P4 assembler flags already
# include PIE and hardware-loop extensions. Keeping the default flags also
# avoids dropping other P4 extensions when the ABI scaffold becomes a real
# PIE implementation.
