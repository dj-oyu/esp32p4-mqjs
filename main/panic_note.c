/*
 * panic_note.c — put the panic reason into the LP SRAM black box.
 *
 * docs/term-design.md §4.4 lists "panic 理由" as SYS-partition content: the
 * point of a flight recorder is that `lastboot` says both what the device was
 * saying and what killed it. The ring already survives a panic reset
 * (PHASE3_MANIFEST.md §1 measured it bit-perfect), and this is the one line
 * that names the cause.
 *
 * WHY A LINKER WRAP. IDF 6 has no user hook on the panic path — panic.c calls
 * nothing weak and nothing registrable, and esp_register_shutdown_handler is
 * NOT called on a panic (it runs on esp_restart, i.e. exactly the case that
 * does not need this). So the only seam is `-Wl,--wrap=esp_panic_handler`,
 * which components/ui_tab5 already uses twice for the same reason. The call
 * comes from panic_handler.c, a different translation unit from panic.c's
 * definition, so the wrap takes effect.
 *
 * WHAT THIS IS NOT. It is not a coredump replacement (CONFIG_ESP_COREDUMP_*
 * exists separately and runs later in the same handler): no backtrace, no
 * registers, no stack. One line, so that a pull of `lastboot` over MQTT is
 * self-explanatory without a serial capture.
 *
 * PANIC CONTEXT. Interrupts are off, the other core is stalled mid-whatever,
 * the heap may be broken and the task that was running may have been half way
 * through a ring append. Everything this function calls is chosen for that:
 * term_lp_panic_note() allocates nothing, takes no lock, revalidates the
 * ring's shadow header and refuses rather than publishing an inconsistent one
 * (see term_lp_ring.h); pcTaskGetName/xTaskGetCurrentTaskHandleForCore is the
 * same idiom IDF's own panic_arch.c uses to name the task; and the note is
 * written BEFORE __real_esp_panic_handler runs, so the printing, the WDT
 * feeding and the coredump cannot get in the way of the one line we want.
 */
#include <stdint.h>

#include "esp_private/panic_internal.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "term_lp_ring.h"

void __real_esp_panic_handler(panic_info_t *info);

static const char *panic_kind(panic_exception_t e)
{
    switch (e) {
    case PANIC_EXCEPTION_DEBUG: return "debug";
    case PANIC_EXCEPTION_IWDT:  return "iwdt";
    case PANIC_EXCEPTION_TWDT:  return "twdt";
    case PANIC_EXCEPTION_ABORT: return "abort";
    case PANIC_EXCEPTION_FAULT: return "fault";
    }
    return "unknown";
}

void __wrap_esp_panic_handler(panic_info_t *info)
{
    if (info) {
        term_lp_panic_t p;
        TaskHandle_t h;

        p.kind = g_panic_abort ? "abort" : panic_kind(info->exception);
        /* esp_panic_handler overrides `reason` for an abort a few lines into
           __real_; do the same here so an abort carries its details rather
           than the illegal-instruction string the trap arrived with. */
        p.reason = g_panic_abort ? g_panic_abort_details : info->reason;
        p.task = NULL;
        p.pc = (uint32_t)(uintptr_t)info->addr;
        p.cause = info->frame ? panic_get_cause(info->frame) : 0u;
        p.core = info->core;

        h = xTaskGetCurrentTaskHandleForCore(info->core);
        if (h)
            p.task = pcTaskGetName(h);

        term_lp_panic_note(&p);
    }
    __real_esp_panic_handler(info);
}
