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

/*
 * Request the current (or next) run to exit. Callable from another
 * FreeRTOS task. Also aborts JS code that is actively executing via
 * the interrupt handler. The flag is sticky so a stop issued just
 * before mqjs_run_script() starts is not lost; the host must call
 * mqjs_runtime_clear_stop() before starting a run it wants to keep.
 */
void mqjs_runtime_stop(void);

/* Re-arm the runtime after a stop request (call before mqjs_run_script). */
void mqjs_runtime_clear_stop(void);

#ifdef __cplusplus
}
#endif
