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

#endif /* CONFIG_MQJS_SSH */

#ifdef __cplusplus
}
#endif
