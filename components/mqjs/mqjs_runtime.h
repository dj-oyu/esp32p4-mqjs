/*
 * mqjs_runtime.h - MicroQuickJS runtime for ESP32-P4 (Stamp-P4)
 *
 * Runs one JS task in a fresh context and pumps an event loop
 * (timers + GPIO interrupts) until nothing remains scheduled.
 */
#pragma once

#include <stdbool.h>
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

/*
 * Post a touch event to the JS event loop (callable from another task,
 * not from an ISR). kind: 0 = down, 1 = move, 2 = up. Coordinates are
 * in the ui canvas space (see ui.size()). Dropped silently when no
 * ui.onTouch handler is registered or the queue is full.
 */
void mqjs_post_touch(int x, int y, int kind);

/*
 * Post one on-screen-keyboard key to the JS event loop (callable from
 * another task, not from an ISR). `utf8` is the key as a short string
 * (1..8 bytes): printable keys verbatim, "\n" enter, "\b" backspace,
 * "\x1b[D" / "\x1b[C" cursor left/right. Dropped silently when no
 * ui.onKey handler is registered or the queue is full.
 */
void mqjs_post_key(const char *utf8, size_t len);

/*
 * Feed SSH session bytes / termination into the JS event loop (callable
 * from the sshc session task, not from an ISR).
 *
 * mqjs_post_ssh_data takes ownership of `data` (heap) on success and the
 * dispatcher frees it; on failure (queue stayed full after a bounded
 * wait) ownership stays with the caller — terminal output must not be
 * dropped, so the caller should retry or tear the session down.
 * mqjs_post_ssh_closed delivers a short human reason once.
 */
bool mqjs_post_ssh_data(char *data, size_t len);
void mqjs_post_ssh_closed(const char *reason);

#ifdef __cplusplus
}
#endif
