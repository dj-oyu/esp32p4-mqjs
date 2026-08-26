/*
 * Host stub of docs/native-editor-spec.md §A.6, used only while
 * components/mqjs/mqjs_native.h does not exist yet.
 *
 * run_tests.sh checks for the real header and prefers it, so the day the
 * runtime lands its own this file stops being compiled — and if the real
 * one disagrees with the spec, the test build breaks here rather than
 * silently testing against a signature nobody ships.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *name;
    const char *title;
    void (*focus)(void *ctx);
    void (*blur)(void *ctx);
    void (*key)(void *ctx, const char *utf8, size_t len,
                uint32_t t_post, uint32_t t_isr);
    void (*touch)(void *ctx, int x, int y, int kind);
    void *ctx;
} mqjs_native_surface_t;

int  mqjs_native_register(const mqjs_native_surface_t *s);
void mqjs_native_focus(int id);
bool mqjs_native_is_fg(int id);
