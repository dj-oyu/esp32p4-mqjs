# ime_core — the IME session policy shared by every input surface.
#
# Pure logic, exactly like skk_core: no I/O, no LVGL, no allocation, no
# clock, so it builds unconditionally on any target and needs no
# REQUIRES beyond the engine it sits on. That is deliberate — the same
# .c is compiled by plain host gcc for tools/tests/test_ime_core.c, and
# the ordering guarantee this component exists to provide is verified
# there rather than on the device.
idf_component_register(SRCS "ime_core.c"
                       INCLUDE_DIRS "include"
                       REQUIRES skk_core)
