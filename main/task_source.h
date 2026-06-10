#pragma once

/* Host-level "task over MQTT" receiver.
 *
 * A dedicated esp-mqtt client (independent from the JS mqtt.* binding)
 * subscribes to CONFIG_MQJS_TASK_TOPIC. A publish to that topic is
 * treated as replacement JS source: it is buffered and the running
 * script is interrupted via mqjs_runtime_stop(). */

/* Start the receiver (no-op when CONFIG_MQJS_TASK_TOPIC is empty). */
void task_source_start(void);

/* Hand over the most recently received script, or NULL. The caller
 * owns the returned buffer (free() it when replaced). */
char *task_source_take(void);
