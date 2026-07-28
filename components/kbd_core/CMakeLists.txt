# Keyboard semantics shared by every input surface (the A164 dock driver
# and the on-screen keyboard / control bar). Pure logic, so it builds
# unconditionally and on any target — the surfaces themselves stay behind
# their own kconfig options.
idf_component_register(SRCS "kbd_core.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_timer)
