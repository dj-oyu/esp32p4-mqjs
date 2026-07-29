/*
 * term_ui_tab5 — the device glue that gives the registry a UI.
 *
 * Device-only (ESP_PLATFORM). This is NOT part of the contract the
 * phase-2 tests are written against: it is the platform wiring that
 * turns the pure registry into something you can look at.
 *
 * One call brings the whole thing up, in the order term_registry.h
 * requires: install the FreeRTOS port, initialise the table, start the
 * reaper task, hook the UI frame. Call it from app_main after
 * ui_tab5_start(), or let the mqjs bindings call it lazily on the first
 * term.* use — it is idempotent either way.
 */
#pragma once

#include "term_registry.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Bring up the native terminal subsystem. `cfg` may be NULL for all
 * defaults (and then no system console is created — pass a cfg with
 * ->console set to get one). Returns TERM_OK, or the registry's error;
 * a second call after a successful one returns TERM_OK and does
 * nothing.
 */
term_err_t term_ui_tab5_start(const term_registry_config_t *cfg);

/* Start the reaper task (§3.1 stage 2). Called by term_ui_tab5_start();
 * exposed for a boot path that wants the registry without the UI. */
void term_port_freertos_start_reaper(void);

#ifdef __cplusplus
}
#endif
