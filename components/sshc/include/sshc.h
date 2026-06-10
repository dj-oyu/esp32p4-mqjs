/*
 * Public C API of the SSH client session (wolfSSH). Backs the JS ssh.*
 * bindings in mqjs_runtime.c.
 *
 * With CONFIG_MQJS_SSH=n every entry point is a no-op inline stub, so
 * mqjs never needs #ifdefs and Stamp builds carry zero SSH code (same
 * trick as ui_tab5.h).
 *
 * Threading: all functions below are called from js_task and never
 * block for long (connect spawns the session task; close waits for the
 * task to die, bounded). The session task owns the socket and the
 * wolfSSH session exclusively; bytes flow
 *   JS -> tx stream buffer -> session task -> server
 *   server -> session task -> mqjs_post_ssh_data() heap copy -> JS
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

#if CONFIG_MQJS_SSH

/* Start a session task: TCP connect + handshake + password auth + shell
 * with pty (cols x rows). Returns false when a session is already
 * running or resources are exhausted. Progress/termination is reported
 * via mqjs_post_ssh_closed(); data via mqjs_post_ssh_data(). */
bool mqjs_ssh_connect(const char *host, int port, const char *user,
                      const char *pass, int cols, int rows);
/* Queue bytes for the server (keystrokes). Returns false when the tx
 * buffer is full or no session is up — the caller may retry. */
bool mqjs_ssh_write(const void *data, size_t len);
/* Request a pty size change (asynchronous). */
void mqjs_ssh_resize(int cols, int rows);
/* Tear the session down. Blocks until the task exited (bounded ~3s);
 * safe to call when nothing is running. */
void mqjs_ssh_close(void);
/* Session task alive (connecting or up) — keeps the JS loop running. */
bool mqjs_ssh_active(void);
/* Shell channel established (auth done). */
bool mqjs_ssh_up(void);

#else /* stubs: SSH disabled */

static inline bool mqjs_ssh_connect(const char *host, int port,
                                    const char *user, const char *pass,
                                    int cols, int rows)
{
    (void)host; (void)port; (void)user; (void)pass; (void)cols; (void)rows;
    return false;
}
static inline bool mqjs_ssh_write(const void *data, size_t len)
{
    (void)data; (void)len;
    return false;
}
static inline void mqjs_ssh_resize(int cols, int rows)
{
    (void)cols; (void)rows;
}
static inline void mqjs_ssh_close(void) {}
static inline bool mqjs_ssh_active(void) { return false; }
static inline bool mqjs_ssh_up(void) { return false; }

#endif /* CONFIG_MQJS_SSH */

#ifdef __cplusplus
}
#endif
