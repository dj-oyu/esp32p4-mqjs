# term_core: the native terminal (docs/term-design.md).
#
#   term_core.c          phase 1 — VT parser + grid + scrollback, pure logic
#   term_port.c          the platform seam holder
#   term_registry.c      phase 2 — slot table, ownership, quiesce (pure logic)
#   term_port_freertos.c device port     (ESP_PLATFORM-guarded)
#   term_ui_tab5.c       drain + blit glue (ESP_PLATFORM-guarded)
#
# The first three build and are tested on the host (host_test/run_tests.sh);
# the last two are guarded so that same runner compiles them to nothing.

idf_component_register(
    SRCS "term_core.c"
         "term_port.c"
         "term_registry.c"
         "term_port_freertos.c"
         "term_ui_tab5.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES ui_tab5 esp_timer
)

# §9.2: IDF appends -fno-jump-tables to every translation unit so that
# jump tables (flash .rodata) can never be reached from IRAM code, and
# the docs sanction re-enabling it per file for sources that do not need
# to be in IRAM. The VT state machine is one large switch per byte and
# runs only on the UI frame task, never from an ISR — exactly the case
# the exemption is written for.
set_source_files_properties(
    term_core.c
    PROPERTIES COMPILE_OPTIONS "-fjump-tables;-ftree-switch-conversion"
)
