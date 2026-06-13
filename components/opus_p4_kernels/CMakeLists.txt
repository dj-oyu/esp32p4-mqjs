idf_component_register(SRCS "opus_p4_kernels.c"
                       INCLUDE_DIRS "include")

target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
