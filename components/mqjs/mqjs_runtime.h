/*
 * mqjs_runtime.h - MicroQuickJS runtime for ESP32-P4 (Stamp-P4 / Tab5)
 *
 * P4a multi-app runtime (docs/launcher-multiapp-design.md): one JS task
 * owns up to MQJS_MAX_APPS cooperative mquickjs contexts. Events and
 * timers are dispatched serially to the owning app's context; UI is
 * exclusive to the foreground app (background ui.* calls are no-ops).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* App slots. Slot 0 is reserved for the resident launcher (P4b); the
 * dev slot keeps the single-task development flow (push -> replace ->
 * auto-rerun) of the pre-P4 runtime. */
#define MQJS_MAX_APPS      4
#define MQJS_SLOT_LAUNCHER 0
#define MQJS_SLOT_DEV      1

/* Fixed per-app context arena (PSRAM), allocated once by mqjs_rt_init
 * and never returned: app_start can then never fail with OOM and the
 * PSRAM heap is not churned (design §3.6). */
#define MQJS_APP_MEM_SIZE (256 * 1024)

/*
 * Allocate the per-slot context arenas and the shared event queue.
 * Call once from the JS task before any mqjs_app_start.
 */
void mqjs_rt_init(void);

/*
 * Compile and run `src`'s top level in a fresh context bound to `slot`,
 * registering its callbacks; the scheduler then keeps dispatching its
 * events. JS-task only. `src` must stay valid until the app stops
 * (bytecode is relocated in place and stays referenced). Returns 0 on
 * success, -1 when the slot is busy / init failed / the top level threw
 * (the slot is clean again in that case).
 */
int mqjs_app_start(int slot, const char *src, size_t src_len,
                   const char *name);

/*
 * Tear one app down: release its callbacks, close the resources it
 * owns (gpio ISRs, its mqtt client, its ssh sessions), destroy its
 * widget screens when it is the foreground app, free the context.
 * In-flight events of a stopped app are dropped by the dispatcher
 * (slot + generation check) — the queue is never drained. JS-task only.
 */
void mqjs_app_stop(int slot);

bool mqjs_app_running(int slot);

/*
 * Dev-slot source provider, called on the JS task whenever the dev slot
 * is idle (first start, 1s after a natural end, immediately after a
 * stop request). Fill src, len and name (the buffer must stay valid
 * until the app stops) and return true to (re)start the dev app; false
 * to stay idle (asked again in 1s).
 */
typedef bool (*mqjs_dev_source_fn)(const char **src, size_t *len,
                                   const char **name, void *user);

/*
 * The multi-app scheduler loop (design §3.7): run all due timers across
 * slots, dispatch one event to its owning context, reap idle slots.
 * Never returns. JS-task only.
 */
void mqjs_runtime_run(mqjs_dev_source_fn next_dev, void *user);

/*
 * Request a foreground switch (queued; the switch runs on the JS task:
 * background callback -> screen/canvas teardown -> foreground callback,
 * design §3.3). Callable from any task (not from an ISR).
 */
void mqjs_focus(int slot);

/*
 * Register a relaunchable app source (embedded buffer, must live
 * forever). sys.launch(name) resolves names against this registry
 * before falling back to /littlefs/apps/<name>.js. The entry named
 * "launcher" is special: the scheduler keeps it resident in slot 0
 * (auto-started, restarted if it ever dies). Call before
 * mqjs_runtime_run.
 */
void mqjs_register_app_source(const char *name, const char *src,
                              size_t len);

/*
 * Ask the launcher to open `name` (focus it if running, launch it
 * otherwise) — the status-bar chip / notification tap entry point.
 * Posts an open request to the resident launcher app; a no-op when no
 * launcher is running. Callable from any task (not from an ISR).
 */
void mqjs_request_open(const char *name);

/*
 * sys.notify(text) sink: receives "[appname] text" lines on the JS
 * task (the host wires this to the status bar). NULL disables.
 */
void mqjs_set_notify_sink(void (*fn)(const char *text));

/*
 * Single-app compatibility wrapper (PC builds: run_pc / test_pc).
 * Runs `src` in the dev slot using `mem_buf` as its arena and pumps the
 * scheduler until no app has anything pending or mqjs_runtime_stop()
 * is called. Returns 0 on success, -1 on JS exception / init failure.
 */
int mqjs_run_script(const char *src, size_t src_len, const char *name,
                    void *mem_buf, size_t mem_size);

/* Request the dev app to stop (callable from another FreeRTOS task).
 * Under mqjs_runtime_run the dev source provider is then asked for the
 * replacement immediately (task push); under mqjs_run_script the pump
 * exits. */
void mqjs_runtime_stop(void);

/*
 * T3c stats panel: copy the current clipboard head (type + data
 * truncated to dcap-1 bytes at a UTF-8 boundary) for display. The only
 * clipboard entry point callable OFF the JS task (mutex-guarded).
 * Returns false when the clipboard is empty (outputs untouched).
 */
bool mqjs_clipboard_peek(char *type, size_t tcap, char *data, size_t dcap);

/*
 * Tee everything js_print / console.log / uncaught-exception dumps write
 * to stdout into `fn` as well, one UTF-8 line per call (no trailing
 * newline; long lines are split at a UTF-8 boundary). Lines are
 * assembled per app so concurrent apps cannot interleave inside a line;
 * non-dev apps are prefixed with "[name] ". `fn` runs on the JS task
 * and must copy + return quickly. NULL disables the tee.
 */
void mqjs_set_print_sink(void (*fn)(const char *line, size_t len));

/*
 * Post a touch event to the JS event loop (callable from another task,
 * not from an ISR). kind: 0 = down, 1 = move, 2 = up. Coordinates are
 * in the ui canvas space (see ui.size()). Touch always belongs to the
 * foreground app; dropped silently when it has no ui.onTouch handler
 * or the queue is full.
 */
void mqjs_post_touch(int x, int y, int kind);

/*
 * Post one on-screen-keyboard key to the JS event loop (callable from
 * another task, not from an ISR). `utf8` is the key as a short string
 * (1..8 bytes): printable keys verbatim, "\n" enter, "\b" backspace,
 * "\x1b[D" / "\x1b[C" cursor left/right. Keys always belong to the
 * foreground app; dropped silently when it has no ui.onKey handler or
 * the queue is full.
 */
void mqjs_post_key(const char *utf8, size_t len);

/*
 * Feed SSH session bytes / termination into the JS event loop (callable
 * from an sshc session task, not from an ISR). `id` is the session
 * handle returned by mqjs_ssh_connect (W3: up to 3 concurrent sessions);
 * the dispatcher routes it to the app that opened the session.
 *
 * mqjs_post_ssh_data takes ownership of `data` (heap) on success and the
 * dispatcher frees it; on failure (queue stayed full after a bounded
 * wait) ownership stays with the caller — terminal output must not be
 * dropped, so the caller should retry or tear the session down.
 * mqjs_post_ssh_closed delivers a short human reason once.
 */
bool mqjs_post_ssh_data(int id, char *data, size_t len);
void mqjs_post_ssh_closed(int id, const char *reason);

#ifdef __cplusplus
}
#endif
