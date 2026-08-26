# edit_core: the editor model (docs/native-editor-spec.md §A.1).
#
#   edit_core.c    the public surface: block layout, text in/out, edits,
#                  cursor, selection, preedit, marks, state
#   edit_buf.c     the gap buffer, the physical line index, UTF-8 walkers
#   edit_undo.c    the byte-capped undo/redo ring
#   edit_lex.c     the per-line lexer and its deferred state cascade
#   edit_view.c    dirty rows, scroll follow, edit_view_row's run cutting
#   edit_check.c   all six invariants, by recount
#
# Pure C99 on purpose: no ESP-IDF header, no malloc, no static mutable state,
# no lock.  The whole component compiles with a host gcc/clang, which is what
# lets host_test/ run it under ASAN/UBSAN and libFuzzer instead of on the one
# device we have (docs/native-editor-plan.md §5).  Keep it that way — anything
# that needs esp_* belongs in components/edit_ui.
#
# There is exactly one file included from outside: ui_cell_width.h, by
# relative path, header-only and itself free of ESP-IDF headers.  It is the
# single source of truth for cell width; components/term_core does the same.
# It is deliberately NOT a component dependency (edit_core links nothing).

idf_component_register(SRCS "edit_core.c"
                            "edit_buf.c"
                            "edit_undo.c"
                            "edit_lex.c"
                            "edit_view.c"
                            "edit_check.c"
                       INCLUDE_DIRS "include")
