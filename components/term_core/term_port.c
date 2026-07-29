/*
 * term_port.c — the installed platform table, and nothing else.
 *
 * docs/term-design.md §11.2. term_port.h explains why the seam exists;
 * this file is only the holder. It deliberately contains no policy: a
 * port is either complete or refused, and the registry never asks which
 * one it got.
 *
 * Host-safe by construction (no ESP-IDF, no FreeRTOS), so the phase-1
 * test runner compiles it along with every other source in the parent
 * directory.
 */
#include "term_port.h"

static const term_port_t *s_port;

bool term_port_valid(const term_port_t *p)
{
    if (!p)
        return false;
    /* Every hook the registry calls unconditionally. reaper_wake and log
     * are the only optional ones (term_port.h "-- optional --"), because
     * correctness must not depend on either: the reaper is a polling
     * pass, and diagnostics are diagnostics. */
    return p->mutex_create && p->mutex_destroy && p->mutex_lock &&
           p->mutex_unlock && p->now_ms && p->signal_create &&
           p->signal_destroy && p->signal_set && p->signal_wait &&
           p->ui_post && p->ui_is_current && p->mem_alloc && p->mem_free;
}

bool term_port_install(const term_port_t *p)
{
    if (!term_port_valid(p))
        return false; /* incomplete table changes nothing */
    s_port = p;       /* retained, not copied — static lifetime required */
    return true;
}

const term_port_t *term_port_get(void)
{
    return s_port;
}

bool term_port_installed(void)
{
    return s_port != NULL;
}
