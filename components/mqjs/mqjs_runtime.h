/*
 * mqjs_runtime.h - MicroQuickJS runtime for ESP32-P4 (Stamp-P4)
 *
 * Runs one JS task in a fresh context and pumps an event loop
 * (timers + GPIO interrupts) until nothing remains scheduled.
 */
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Compile and run `src` in a brand-new context allocated inside
 * `mem_buf` (the engine never allocates outside this buffer).
 * Blocks until the script finishes AND no timers / gpio handlers
 * remain, or until mqjs_runtime_stop() is called from another task.
 *
 * Returns 0 on success, -1 on JS exception or init failure.
 */
int mqjs_run_script(const char *src, size_t src_len, const char *name,
                    void *mem_buf, size_t mem_size);

/* Request the event loop to exit (callable from another FreeRTOS task). */
void mqjs_runtime_stop(void);

/*
 * Tee everything js_print / console.log / uncaught-exception dumps write
 * to stdout into `fn` as well, one UTF-8 line per call (no trailing
 * newline; long lines are split at a UTF-8 boundary). `fn` runs on the
 * JS task and must copy + return quickly. NULL disables the tee.
 */
void mqjs_set_print_sink(void (*fn)(const char *line, size_t len));

#ifdef __cplusplus
}
#endif
