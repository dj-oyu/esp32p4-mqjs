#pragma once

#include <stdbool.h>
#include <stddef.h>

/* Host-level "task over MQTT" receiver.
 *
 * A dedicated esp-mqtt client (independent from the JS mqtt.* binding)
 * subscribes to CONFIG_MQJS_TASK_TOPIC. A publish to that topic is
 * treated as replacement JS source: it is buffered and the running
 * script is interrupted via mqjs_runtime_stop(). */

/* Start the receiver (no-op when CONFIG_MQJS_TASK_TOPIC is empty). */
void task_source_start(void);

/* Hand over the most recently received task, or NULL. *len receives
 * its size (tasks may be bytecode, so NUL-termination cannot be relied
 * on for length). The caller owns the returned buffer. */
char *task_source_take(size_t *len);

/* ---- §11 store catalog (launcher-multiapp-design) ----
 * The broker's retained <topic>/store/<name> rows (manifest headers)
 * are mirrored into a small in-RAM table; payloads on apps/<name> are
 * subscribed per installed app only. All four are safe to call before
 * task_source_start (count 0 / false / no-op). */
int task_source_store_count(void);
bool task_source_store_get(int idx, char *name, size_t ncap, char *head,
                           size_t hcap);
/* Subscribe the app's payload topic = install request; the retained
 * body arrives async and lands via the signed registry path. */
bool task_source_install(const char *name);
/* Drop the payload subscription after a local uninstall so the
 * retained body does not reinstall it on the next sync. */
void task_source_app_unsub(const char *name);
