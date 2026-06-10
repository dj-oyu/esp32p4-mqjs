cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32p4-mqjs)

# wolfSSL 5.8.2's esp_sdk_mem_lib.c declares an enum member named
# 'thread_local', which became a reserved keyword in C23. IDF 6 compiles
# with -std=gnu23, so that file fails to build. Recompile the wolfSSL /
# wolfSSH components as gnu17 (the last -std on the GCC command line wins),
# where 'thread_local' is an ordinary identifier again. Applies to both
# Tab5 (links SSH) and Stamp (compiles but gc-sections drops it).
foreach(_wolf_lib __idf_wolfssl__wolfssl __idf_wolfssl__wolfssh)
    if(TARGET ${_wolf_lib})
        target_compile_options(${_wolf_lib} PRIVATE -std=gnu17)
    endif()
endforeach()
