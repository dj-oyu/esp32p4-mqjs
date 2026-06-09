/*
 * script_runner.h - JS task lifecycle, fed by the MQTT transport.
 *
 * The runner task owns the JS context. Other tasks (MQTT) hand it a
 * new script or a control command; the runner aborts the current run
 * and applies the change.
 */
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Hard cap on a distributed script (must fit the NVS partition and
   leave room inside the 256KB JS heap). */
#define MQJS_SCRIPT_MAX (64 * 1024)

typedef enum {
    SCRIPT_CMD_NONE = 0,
    SCRIPT_CMD_RESTART,  /* re-run the current script from scratch   */
    SCRIPT_CMD_STOP,     /* abort and stay idle until the next event */
    SCRIPT_CMD_CLEAR,    /* drop the current script and stay idle    */
} script_cmd_t;

/* malloc that prefers PSRAM and falls back to internal RAM */
void *script_psram_alloc(size_t size);

void script_runner_init(void);

/* Hand over a NUL-terminated heap buffer (ownership moves to the
   runner, which will free() it). The running script is aborted and
   replaced. */
void script_runner_submit(char *src, size_t len);

void script_runner_control(script_cmd_t cmd);

#ifdef __cplusplus
}
#endif
