# term_core: the native terminal (docs/term-design.md).
#
#   term_core.c          phase 1 — VT parser + grid + scrollback, pure logic
#   term_port.c          the platform seam holder
#   term_registry.c      phase 2 — slot table, ownership, quiesce (pure logic)
#   term_port_freertos.c device port     (ESP_PLATFORM-guarded)
#   term_ui_tab5.c       drain + blit glue (ESP_PLATFORM-guarded)
#   term_lp_ring.c       phase 3 — the LP SRAM black box (§4.4). The ring
#                        itself is pure logic over a caller-supplied region
#                        and builds everywhere; only the RTC_NOINIT region,
#                        the reset cause, the clock and the PSRAM snapshot
#                        allocator are guarded. The 31.5 KiB region is linked
#                        unconditionally — it is the whole point of the file,
#                        and the linker ASSERTs the LP RAM fit rather than
#                        relocating it silently.
#   term_bb_pull.c       phase 3 — the signed black-box pull (§7.2). Pure: the
#                        whole responder runs over four callbacks, so there is
#                        nothing to guard here and nothing device-specific to
#                        link. The MQTT/NVS/TweetNaCl glue lives in
#                        main/task_source.c, where the connection is owned.
#
# term_core.c, term_port.c, term_registry.c, term_lp_ring.c and
# term_bb_pull.c build and are tested on the host (host_test/run_tests.sh);
# the two device-glue files are guarded so that same runner compiles them to
# nothing.
#
# The phase-3 prerequisite retention probe (term_lp_probe.*, commit 77b1c99)
# was removed here: it claimed the same LP region as the ring, and the ring's
# boot marker measures retention permanently on real data instead
# (PHASE3_MANIFEST.md §2).

idf_component_register(
    SRCS "term_core.c"
         "term_port.c"
         "term_registry.c"
         "term_port_freertos.c"
         "term_ui_tab5.c"
         "term_lp_ring.c"
         "term_bb_pull.c"
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
