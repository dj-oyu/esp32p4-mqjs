# SSH client session (wolfSSH). Sources compile only with CONFIG_MQJS_SSH=y;
# PRIV_REQUIRES stays unconditional (requirement expansion runs before
# kconfig), so disabled builds still fetch+compile wolfSSL but link none of
# it — flash size is unaffected (same pattern as ui_tab5's LVGL).
if(CONFIG_MQJS_SSH)
    set(SSHC_SRCS "sshc.c")
else()
    set(SSHC_SRCS "")
endif()

idf_component_register(SRCS ${SSHC_SRCS}
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES lwip wolfssl__wolfssl wolfssl__wolfssh)
