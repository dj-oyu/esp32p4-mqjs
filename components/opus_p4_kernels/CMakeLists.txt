set(OPUS_P4_KERNEL_SRCS "opus_p4_kernels.c")

# Self-guards to ESP32-P4; harmless on other targets. Used to confirm PIE
# instruction semantics on silicon before building the PIE kernels.
list(APPEND OPUS_P4_KERNEL_SRCS "opus_p4_pie_probe.S")
list(APPEND OPUS_P4_KERNEL_SRCS "opus_p4_comb8.S")
list(APPEND OPUS_P4_KERNEL_SRCS "opus_p4_denorm8.S")
list(APPEND OPUS_P4_KERNEL_SRCS "opus_p4_normres8.S")
list(APPEND OPUS_P4_KERNEL_SRCS "opus_p4_bfly3_4.S")
list(APPEND OPUS_P4_KERNEL_SRCS "opus_p4_bfly3_8.S")

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
