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
        # -std=gnu17: see thread_local note above.
        # -Wno-error=maybe-uninitialized: enabling the wolfSSH terminal-resize
        #   path (below) compiles SendChannelTerminalResize, which trips this
        #   warning in upstream code; don't let it fail the build.
        target_compile_options(${_wolf_lib} PRIVATE
            -std=gnu17 -Wno-error=maybe-uninitialized)
    endif()
endforeach()

# Enable wolfSSH's terminal window-change API (wolfSSH_ChangeTerminalSize) by
# undefining NO_FILESYSTEM, which the managed wolfSSL user_settings.h sets and
# which gates that API off. The shim header (components/sshc/wolfssl_override/
# user_settings.h) pulls the real settings via #include_next then #undefs it;
# placing its dir BEFORE on the include path makes wolfSSL/wolfSSH/sshc see it
# first. This lets the Tab5 terminal resize the server's pty to match the
# on-screen grid (and stays orientation-agnostic — the pty follows whatever
# cols/rows the screen geometry yields, so a future landscape mode just works).
set(_wolf_override ${CMAKE_CURRENT_SOURCE_DIR}/components/sshc/wolfssl_override)
foreach(_wolf_tgt __idf_wolfssl__wolfssl __idf_wolfssl__wolfssh __idf_sshc)
    if(TARGET ${_wolf_tgt})
        target_include_directories(${_wolf_tgt} BEFORE PRIVATE ${_wolf_override})
    endif()
endforeach()
