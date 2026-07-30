/*
 * Public C API of the SSH client sessions (wolfSSH), W3: handle-based,
 * up to SSHC_MAX_SESSIONS concurrent. Backs the JS ssh.* bindings in
 * mqjs_runtime.c.
 *
 * With CONFIG_MQJS_SSH=n every entry point is a no-op inline stub, so
 * mqjs never needs #ifdefs and Stamp builds carry zero SSH code (same
 * trick as ui_tab5.h).
 *
 * Threading: all functions below are called from js_task and never
 * block for long (connect spawns a session task; close waits for the
 * task to die, bounded). Each session task owns its socket and wolfSSH
 * session exclusively; bytes flow
 *   JS -> per-session tx stream buffer -> session task -> server
 *   server -> session task -> mqjs_post_ssh_data(id, ...) heap copy -> JS
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Concurrent session cap (each costs ~8KB task stack + wolfSSH state +
 * crypto buffers of internal RAM — design §7: 2-3, tunable here). */
#define SSHC_MAX_SESSIONS 3

/*
 * A receive SINK: somewhere for this session's bytes to go that is not the
 * JS heap (docs/term-design.md §5, term.pipe). While one is installed the
 * session task hands received bytes straight to it and
 * mqjs_post_ssh_data() is not called at all — no malloc per chunk, no
 * event, no JS string. sshc knows nothing about terminals; the sink is
 * three function pointers.
 *
 * ALL THREE RUN ON THE SESSION TASK and must not block.
 *
 *   space()  bytes the sink can take right now. The session task reads AT
 *            MOST that many off the wire, so write() can never come up
 *            short and an escape sequence is never cut in half (§5). When
 *            it answers 0 the session STOPS READING for one recv timeout:
 *            the socket buffer fills, the TCP window closes and the peer's
 *            SSH window stops advancing — the backpressure §5 asks for,
 *            produced by not consuming rather than by blocking anybody.
 *   write()  take the bytes; returns how many were taken.
 *   gone()   the sink has been released and this session will never touch
 *            it again. Called exactly once per installed sink, from the
 *            session task, either because mqjs_ssh_drop_sink() asked or
 *            because the session ended. It is the detach ACK the term
 *            registry's quiesce protocol waits for (§3.1), which is why
 *            it is mandatory and why only the session task ever calls it.
 */
typedef struct {
    size_t (*space)(void *user);
    size_t (*write)(void *user, const void *data, size_t len);
    void   (*gone)(void *user);
    void   *user;
} sshc_sink_t;

#if CONFIG_MQJS_SSH

/* Start a session task: TCP connect + handshake + password auth + shell
 * with pty (cols x rows). Returns the session id (> 0), or 0 when all
 * session slots are busy / resources exhausted. Progress/termination is
 * reported via mqjs_post_ssh_closed(id, ...); data via
 * mqjs_post_ssh_data(id, ...). Stale ids are safe no-ops everywhere. */
int mqjs_ssh_connect(const char *host, int port, const char *user,
                     const char *pass, const char *hostkey, int cols,
                     int rows);
/* Queue bytes for the server (keystrokes). Returns false when the tx
 * buffer is full or the session is not up — the caller may retry. */
bool mqjs_ssh_write(int id, const void *data, size_t len);
/* Request a pty size change (asynchronous). */
void mqjs_ssh_resize(int id, int cols, int rows);
/* Tear one session down. Blocks until its task exited (bounded ~15s
 * worst case during connect); safe with stale ids. */
void mqjs_ssh_close(int id);
/* Tear all sessions down (task-switch cleanup). */
void mqjs_ssh_close_all(void);
/* Any session task alive — keeps the JS loop running. */
bool mqjs_ssh_active(void);
/* Shell channel established (auth done) for this id. */
bool mqjs_ssh_up(int id);
/* The id names a session slot, whether or not it is up yet. Together with
   mqjs_ssh_up() this separates "still handshaking" (retry) from "no such
   session" (caller bug) — term_pipe_bind needs that distinction. */
bool mqjs_ssh_known(int id);

/* Install the rx sink (see sshc_sink_t). Callable from any task; false
 * for a stale id, a session that is on its way out, or when a sink is
 * already installed — one sink per session, replacing needs a drop and
 * its gone() first (the same single-producer rule the term ring has). */
bool mqjs_ssh_set_sink(int id, const sshc_sink_t *sink);
/* Ask the session task to release its sink. Returns immediately and
 * joins nothing; gone() arrives on the session task within one recv
 * timeout. A stale id is a no-op — a dead session has already called
 * gone(). */
void mqjs_ssh_drop_sink(int id);

#else /* stubs: SSH disabled */

static inline int mqjs_ssh_connect(const char *host, int port,
                                   const char *user, const char *pass,
                                   const char *hostkey, int cols, int rows)
{
    (void)host; (void)port; (void)user; (void)pass; (void)hostkey;
    (void)cols; (void)rows;
    return 0;
}
static inline bool mqjs_ssh_write(int id, const void *data, size_t len)
{
    (void)id; (void)data; (void)len;
    return false;
}
static inline void mqjs_ssh_resize(int id, int cols, int rows)
{
    (void)id; (void)cols; (void)rows;
}
static inline void mqjs_ssh_close(int id) { (void)id; }
static inline void mqjs_ssh_close_all(void) {}
static inline bool mqjs_ssh_active(void) { return false; }
static inline bool mqjs_ssh_up(int id) { (void)id; return false; }
static inline bool mqjs_ssh_set_sink(int id, const sshc_sink_t *sink)
{
    (void)id; (void)sink;
    return false;
}
static inline void mqjs_ssh_drop_sink(int id) { (void)id; }

#endif /* CONFIG_MQJS_SSH */

#ifdef __cplusplus
}
#endif
