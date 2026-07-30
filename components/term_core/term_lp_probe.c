/*
 * term_lp_probe.c — LP SRAM retention probe (docs/term-design.md §11 item 3).
 * Contract, rationale and the report format: term_lp_probe.h.
 *
 * Layout of this file:
 *   - pure logic (CRC32, the PRNG pattern, the survival check). Compiles and
 *     runs anywhere; this is the part a host build can exercise.
 *   - the state store. NVS on the device; an inert stub off it, so the logic
 *     above still links into the host test binaries and the PC build (the
 *     host_test runner compiles every source in this directory, and an
 *     ESP_PLATFORM guard around the whole file would leave the pure logic
 *     unbuilt and the entry points undefined).
 *   - the triggers and the trigger task. Device-only.
 *
 * The one rule that shapes everything: the region under test cannot also be
 * the sequencer's memory. Every state advance is written to NVS and committed
 * before the crash it enables.
 */
#include "term_lp_probe.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "term_lp";
#define LPLOGI(fmt, ...) ESP_LOGI(TAG, fmt, ##__VA_ARGS__)
#define LPLOGW(fmt, ...) ESP_LOGW(TAG, fmt, ##__VA_ARGS__)
#define LPLOGE(fmt, ...) ESP_LOGE(TAG, fmt, ##__VA_ARGS__)
#else
#define LPLOGI(fmt, ...) do { } while (0)
#define LPLOGW(fmt, ...) do { } while (0)
#define LPLOGE(fmt, ...) do { } while (0)
#endif

/* Never a TU without a definition: the host runner links this file into every
   suite, and an empty translation unit is not valid C. */
typedef int term_lp_probe_tu_t;

/* ------------------------------------------------------------------ */
/* the payload under test                                             */
/* ------------------------------------------------------------------ */

#define LP_MAGIC 0x4c505231u   /* "LPR1" */

/* 32-byte header + pattern == exactly TERM_LP_PROBE_REGION_BYTES. hdr_crc
   covers the 16 bytes before it, so a corrupt header is distinguishable from
   a corrupt body — a wipe and a stray write look nothing alike and the design
   cares which one it is facing. */
typedef struct {
    uint32_t magic;
    uint32_t seq;
    uint32_t len;                                  /* == PATTERN_BYTES */
    uint32_t crc;                                  /* crc32 of pattern[] */
    uint32_t hdr_crc;                              /* crc32 of the 16 above */
    uint32_t pad[3];
    uint8_t  pattern[TERM_LP_PROBE_PATTERN_BYTES];
} lp_payload_t;

/* C99 has no _Static_assert (host_test builds with -std=c99). */
typedef char lp_payload_size_check[
    (sizeof(lp_payload_t) == TERM_LP_PROBE_REGION_BYTES) ? 1 : -1];
typedef char lp_pattern_align_check[
    (TERM_LP_PROBE_PATTERN_BYTES % 4u == 0u) ? 1 : -1];

#ifdef ESP_PLATFORM
/* The whole point: an uninitialised region in LP SRAM. The linker places
   .rtc_noinit in lp_ram_seg; if this ever lands anywhere else the probe is
   measuring the wrong memory, so the .map placement is checked as part of
   reading the result (see PHASE3_MANIFEST.md). */
static RTC_NOINIT_ATTR lp_payload_t s_lp;
#else
static lp_payload_t s_lp;      /* ordinary .bss; retains nothing, by design */
#endif

/* One verdict per cause, as stored in the NVS result blob. Fixed layout: the
   blob is written by one firmware and read by the next boot of the same one,
   but a reflash in between is normal, so keep it POD and versioned by
   LP_RES_VERSION rather than by luck. */
#define LP_RES_VERSION 1

#define LP_F_MAGIC   0x01u
#define LP_F_LEN     0x02u
#define LP_F_HDR     0x04u
#define LP_F_CRC     0x08u
#define LP_F_NORESET 0x10u

typedef struct {
    uint8_t  version;
    uint8_t  expected;         /* term_lp_cause_t */
    uint8_t  observed;         /* esp_reset_reason(); 0xff = none seen */
    uint8_t  flags;            /* LP_F_* */
    uint32_t seq_expected;     /* from NVS — authoritative */
    uint32_t seq_seen;         /* from the region — may be garbage */
    uint32_t match_bytes;
    uint32_t nonzero_bytes;
    int32_t  first_bad;
    uint32_t region_bytes;
} lp_verdict_t;

#define LP_RES_MAX TERM_LP_CAUSE_COUNT

/* ------------------------------------------------------------------ */
/* pure logic                                                          */
/* ------------------------------------------------------------------ */

/* CRC32 (IEEE 802.3, reflected) — the same polynomial esp_rom_crc32_le uses,
   spelled out here so the check is identical on the host and on the device
   and so this file needs no ROM symbol to be host-buildable. */
static uint32_t lp_crc32(uint32_t crc, const uint8_t *p, size_t n)
{
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
    }
    return ~crc;
}

/* xorshift32. A PRNG pattern rather than a constant one so that a region
   which "survived" cannot in fact be stale data from an earlier stage: each
   stage seeds from a fresh random seq, and the expected stream is regenerated
   from the seq that NVS remembers, not from the seq the region claims. */
static uint32_t lp_xs32(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

static uint32_t lp_seed(uint32_t seq)
{
    return (seq * 2654435761u) | 1u;   /* xorshift dies at zero */
}

/* Fill the region for a stage. Writes the pattern first and the header last,
   so a reset in the middle of this cannot leave a valid header over a
   half-written body. */
static void lp_fill(uint32_t seq)
{
    uint32_t st = lp_seed(seq);
    for (size_t i = 0; i < TERM_LP_PROBE_PATTERN_BYTES; i += 4) {
        uint32_t w = lp_xs32(&st);
        s_lp.pattern[i + 0] = (uint8_t)(w);
        s_lp.pattern[i + 1] = (uint8_t)(w >> 8);
        s_lp.pattern[i + 2] = (uint8_t)(w >> 16);
        s_lp.pattern[i + 3] = (uint8_t)(w >> 24);
    }
    s_lp.pad[0] = s_lp.pad[1] = s_lp.pad[2] = 0;
    s_lp.magic = LP_MAGIC;
    s_lp.seq = seq;
    s_lp.len = TERM_LP_PROBE_PATTERN_BYTES;
    s_lp.crc = lp_crc32(0, s_lp.pattern, TERM_LP_PROBE_PATTERN_BYTES);
    s_lp.hdr_crc = lp_crc32(0, (const uint8_t *)&s_lp, 16);
}

/* Judge the region against `seq_expected`. Partial survival is a distinct
   verdict from full survival, which is why this reports four independent
   header/body bits plus a byte count and a first-mismatch offset instead of
   one boolean. */
static void lp_check(uint32_t seq_expected, lp_verdict_t *v)
{
    v->seq_expected = seq_expected;
    v->seq_seen = s_lp.seq;
    v->region_bytes = TERM_LP_PROBE_REGION_BYTES;
    v->flags = 0;

    if (s_lp.magic == LP_MAGIC)
        v->flags |= LP_F_MAGIC;
    if (s_lp.len == TERM_LP_PROBE_PATTERN_BYTES)
        v->flags |= LP_F_LEN;
    if (lp_crc32(0, (const uint8_t *)&s_lp, 16) == s_lp.hdr_crc)
        v->flags |= LP_F_HDR;
    if ((v->flags & LP_F_LEN) &&
        lp_crc32(0, s_lp.pattern, TERM_LP_PROBE_PATTERN_BYTES) == s_lp.crc)
        v->flags |= LP_F_CRC;

    uint32_t st = lp_seed(seq_expected);
    uint32_t match = 0, nonzero = 0;
    int32_t first_bad = -1;
    for (size_t i = 0; i < TERM_LP_PROBE_PATTERN_BYTES; i += 4) {
        uint32_t w = lp_xs32(&st);
        for (int k = 0; k < 4; k++) {
            uint8_t got = s_lp.pattern[i + (size_t)k];
            uint8_t want = (uint8_t)(w >> (8 * k));
            if (got)
                nonzero++;
            if (got == want)
                match++;
            else if (first_bad < 0)
                first_bad = (int32_t)(i + (size_t)k);
        }
    }
    v->match_bytes = match;
    v->nonzero_bytes = nonzero;
    v->first_bad = first_bad;
}

static const char *lp_cause_name(unsigned c)
{
    switch (c) {
    case TERM_LP_CAUSE_PANIC:    return "panic";
    case TERM_LP_CAUSE_TASK_WDT: return "task_wdt";
    case TERM_LP_CAUSE_INT_WDT:  return "int_wdt";
    case TERM_LP_CAUSE_RESTART:  return "restart";
    default:                     return "?";
    }
}

/* esp_reset_reason_t by value. Spelled numerically on purpose: this table is
   also compiled off-device, and the numbers are the stable part of that enum
   (0..15 in IDF 6.0). `obs_id` in the report is the authority; the name is a
   convenience. */
static const char *lp_reason_name(unsigned r)
{
    switch (r) {
    case 0:  return "unknown";
    case 1:  return "poweron";
    case 2:  return "ext";
    case 3:  return "sw";
    case 4:  return "panic";
    case 5:  return "int_wdt";
    case 6:  return "task_wdt";
    case 7:  return "wdt";
    case 8:  return "deepsleep";
    case 9:  return "brownout";
    case 10: return "sdio";
    case 11: return "usb";
    case 12: return "jtag";
    case 13: return "efuse";
    case 14: return "pwr_glitch";
    case 15: return "cpu_lockup";
    case 0xff: return "none";
    default: return "?";
    }
}

/* ------------------------------------------------------------------ */
/* state store: NVS on the device, inert off it                        */
/* ------------------------------------------------------------------ */

#define LP_NS     "termlp"
#define LP_K_STEP "step"    /* u8: present => a sequence is in progress */
#define LP_K_PEND "pend"    /* u8: 1 => a cause was fired, judge on boot */
#define LP_K_SEQ  "seq"     /* u32: the seed written into the region */
#define LP_K_RES  "res"     /* blob: lp_verdict_t[] */

#ifdef ESP_PLATFORM

static nvs_handle_t s_nvs;
static bool s_nvs_open;

static bool store_ready(void)
{
    if (s_nvs_open)
        return true;
    /* Same lazy pattern the rest of the runtime uses: wifi.c normally ran
       nvs_flash_init already, but this runs before it and must not depend on
       boot order. */
    if (nvs_open(LP_NS, NVS_READWRITE, &s_nvs) != ESP_OK) {
        nvs_flash_init();
        if (nvs_open(LP_NS, NVS_READWRITE, &s_nvs) != ESP_OK)
            return false;
    }
    s_nvs_open = true;
    return true;
}

static bool store_get_u8(const char *k, uint8_t *out)
{
    return nvs_get_u8(s_nvs, k, out) == ESP_OK;
}

static bool store_set_u8(const char *k, uint8_t v)
{
    return nvs_set_u8(s_nvs, k, v) == ESP_OK;
}

static bool store_get_u32(const char *k, uint32_t *out)
{
    return nvs_get_u32(s_nvs, k, out) == ESP_OK;
}

static bool store_set_u32(const char *k, uint32_t v)
{
    return nvs_set_u32(s_nvs, k, v) == ESP_OK;
}

static void store_erase(const char *k)
{
    nvs_erase_key(s_nvs, k);       /* NOT_FOUND is the normal case */
}

static bool store_commit(void)
{
    return nvs_commit(s_nvs) == ESP_OK;
}

static int store_get_res(lp_verdict_t *out, int max)
{
    size_t sz = 0;
    if (nvs_get_blob(s_nvs, LP_K_RES, NULL, &sz) != ESP_OK)
        return 0;
    int n = (int)(sz / sizeof(lp_verdict_t));
    if (n <= 0)
        return 0;
    if (n > max)
        n = max;
    sz = (size_t)n * sizeof(lp_verdict_t);
    if (nvs_get_blob(s_nvs, LP_K_RES, out, &sz) != ESP_OK)
        return 0;
    return n;
}

static bool store_set_res(const lp_verdict_t *v, int n)
{
    return nvs_set_blob(s_nvs, LP_K_RES, v,
                        (size_t)n * sizeof(lp_verdict_t)) == ESP_OK;
}

static uint32_t lp_random(void)
{
    return esp_random();
}

static unsigned lp_reset_reason(void)
{
    return (unsigned)esp_reset_reason();
}

#else  /* !ESP_PLATFORM */

/* Off the device there is nothing to retain and nothing to crash, so the
   store reports "unavailable" and every entry point stays inert. The pure
   logic above is still compiled and linked, which is the point of building
   this file at all on the host. */
static bool store_ready(void)                        { return false; }
static bool store_get_u8(const char *k, uint8_t *o)  { (void)k; (void)o; return false; }
static bool store_set_u8(const char *k, uint8_t v)   { (void)k; (void)v; return false; }
static bool store_get_u32(const char *k, uint32_t *o){ (void)k; (void)o; return false; }
static bool store_set_u32(const char *k, uint32_t v) { (void)k; (void)v; return false; }
static void store_erase(const char *k)               { (void)k; }
static bool store_commit(void)                       { return false; }
static int  store_get_res(lp_verdict_t *o, int max)  { (void)o; (void)max; return 0; }
static bool store_set_res(const lp_verdict_t *v, int n) { (void)v; (void)n; return false; }
static uint32_t lp_random(void)                      { return 0x9e3779b9u; }
static unsigned lp_reset_reason(void)                { return 0; }

#endif /* ESP_PLATFORM */

static bool store_append_res(const lp_verdict_t *v)
{
    lp_verdict_t buf[LP_RES_MAX + 1];
    int n = store_get_res(buf, LP_RES_MAX);
    if (n >= LP_RES_MAX)
        return false;              /* full: a stale blob, refuse to grow it */
    buf[n] = *v;
    return store_set_res(buf, n + 1);
}

/* ------------------------------------------------------------------ */
/* triggers                                                            */
/* ------------------------------------------------------------------ */

/* Task WDT timeout is 5 s in sdkconfig.tab5; four timeouts is plenty of
   evidence that it is not going to reset us. */
#define LP_TWDT_SPIN_MS   20000
/* Interrupt WDT timeout is 300 ms. */
#define LP_INTWDT_SPIN_MS 5000

#ifdef ESP_PLATFORM
static volatile uint32_t s_sink;       /* keeps the faulting load live */
/* Zero, but laundered through a volatile so the compiler cannot prove it and
   fold the dereference into a trap or delete it outright — a literal *(int*)0
   is undefined behaviour the optimiser is allowed to reinterpret, and this
   probe needs the *hardware* exception, not GCC's opinion of one. */
static volatile uintptr_t s_null_addr;

/* Spin for `ms` without yielding and without needing interrupts: the ROM
   delay is a busy wait on the cycle counter, so it works with interrupts
   masked (int WDT case) and starves the idle task when they are not (task
   WDT case). Both spins are bounded — a cause that does not reset the chip
   must produce a verdict, not a hang. */
static void lp_spin_ms(uint32_t ms)
{
    for (uint32_t i = 0; i < ms; i++)
        esp_rom_delay_us(1000);
}

/* Returns only when the cause failed to reset the device. */
static void lp_fire(unsigned cause)
{
    switch (cause) {
    case TERM_LP_CAUSE_PANIC: {
        volatile uint32_t *p = (volatile uint32_t *)s_null_addr;
        s_sink = *p;              /* LoadProhibited -> panic -> reboot */
        break;
    }
    case TERM_LP_CAUSE_TASK_WDT:
        /* Subscribe this task and never feed it, and starve the idle tasks
           at the same time by never yielding. Note sdkconfig.tab5 has
           CONFIG_ESP_TASK_WDT_PANIC unset, so the expected outcome here is a
           printed warning and NO reset; the bounded spin turns that into a
           recorded "noreset" verdict instead of a hung device. */
        esp_task_wdt_add(NULL);
        lp_spin_ms(LP_TWDT_SPIN_MS);
        esp_task_wdt_delete(NULL);
        break;
    case TERM_LP_CAUSE_INT_WDT:
        portDISABLE_INTERRUPTS();
        lp_spin_ms(LP_INTWDT_SPIN_MS);
        portENABLE_INTERRUPTS();
        break;
    case TERM_LP_CAUSE_RESTART:
        esp_restart();
        break;
    default:
        break;
    }
}
#endif /* ESP_PLATFORM — off the device there is nothing to crash, and the
          trigger task that would call lp_fire() does not exist either */

/* ------------------------------------------------------------------ */
/* sequencer                                                           */
/* ------------------------------------------------------------------ */

static void lp_log_verdict(const lp_verdict_t *v)
{
    (void)v;                       /* the log macros vanish off the device */
    LPLOGW("verdict %s: reset=%s(%u) magic=%d len=%d hdr=%d crc=%d "
           "match=%u/%u first_bad=%d nonzero=%u seq=%u/%u%s",
           lp_cause_name(v->expected), lp_reason_name(v->observed),
           (unsigned)v->observed,
           (v->flags & LP_F_MAGIC) ? 1 : 0, (v->flags & LP_F_LEN) ? 1 : 0,
           (v->flags & LP_F_HDR) ? 1 : 0, (v->flags & LP_F_CRC) ? 1 : 0,
           (unsigned)v->match_bytes, (unsigned)TERM_LP_PROBE_PATTERN_BYTES,
           (int)v->first_bad, (unsigned)v->nonzero_bytes,
           (unsigned)v->seq_seen, (unsigned)v->seq_expected,
           (v->flags & LP_F_NORESET) ? " NO-RESET" : "");
}

/* Make `step` live: fresh pattern in the region, state committed. pend stays
   0 here — it is set immediately before the crash, so a reset that happens
   during the delay window does not get misattributed to the cause. */
static bool lp_stage(uint8_t step)
{
    uint32_t seq = lp_random() | 1u;
    lp_fill(seq);
    if (!store_set_u8(LP_K_STEP, step) || !store_set_u32(LP_K_SEQ, seq) ||
        !store_set_u8(LP_K_PEND, 0) || !store_commit()) {
        LPLOGE("NVS write failed; standing down");
        return false;
    }
    LPLOGW("staged step %u (%s): %u B at %p, seq=%u, firing in %d s",
           (unsigned)step, lp_cause_name(step),
           (unsigned)TERM_LP_PROBE_REGION_BYTES, (void *)&s_lp,
           (unsigned)seq, TERM_LP_PROBE_DELAY_S);
    return true;
}

static void lp_finish(void)
{
    store_erase(LP_K_STEP);
    store_erase(LP_K_PEND);
    store_commit();
    LPLOGW("sequence complete; %d verdicts held in NVS (sys.lpProbe())",
           TERM_LP_CAUSE_COUNT);
}

#ifdef ESP_PLATFORM
static bool s_task_live;

static void lp_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint8_t step = 0;
        if (!store_get_u8(LP_K_STEP, &step) || step >= TERM_LP_CAUSE_COUNT)
            break;                          /* cleared or finished */

        for (int left = TERM_LP_PROBE_DELAY_S; left > 0; left -= 5) {
            LPLOGW("firing %s in %d s (reflash/clear window)",
                   lp_cause_name(step), left);
            vTaskDelay(pdMS_TO_TICKS(5000));
        }

        /* Late abort: term_lp_probe_clear() during the window removes the
           state key, and the sequence stands down instead of crashing. */
        uint8_t still = 0;
        if (!store_get_u8(LP_K_STEP, &still) || still != step) {
            LPLOGW("stood down before firing %s", lp_cause_name(step));
            break;
        }

        /* The crash may take the region with it, so the fact that this cause
           was fired must be on flash BEFORE it is fired. */
        if (!store_set_u8(LP_K_PEND, 1) || !store_commit()) {
            LPLOGE("could not commit pend; standing down");
            break;
        }
        LPLOGE("firing %s NOW", lp_cause_name(step));
        lp_fire(step);

        /* Only reached when the cause did not reset the device. The region is
           still whatever lp_stage wrote, so the survival fields would be a
           tautology — they are left at zero and NORESET says why. */
        lp_verdict_t v;
        memset(&v, 0, sizeof v);
        v.version = LP_RES_VERSION;
        v.expected = step;
        v.observed = 0xff;
        v.flags = LP_F_NORESET;
        v.region_bytes = TERM_LP_PROBE_REGION_BYTES;
        store_get_u32(LP_K_SEQ, &v.seq_expected);
        store_append_res(&v);
        store_set_u8(LP_K_PEND, 0);
        store_commit();
        lp_log_verdict(&v);

        step++;
        if (step >= TERM_LP_CAUSE_COUNT) {
            lp_finish();
            break;
        }
        if (!lp_stage(step))
            break;
    }
    s_task_live = false;
    vTaskDelete(NULL);
}

static bool lp_task_running(void) { return s_task_live; }

static void lp_task_start(void)
{
    if (s_task_live)
        return;
    s_task_live = true;
    /* Core 0 on purpose: the int-WDT case masks interrupts on the core it
       runs on, and core 1 is the LVGL/display core. Priority above idle so
       the task-WDT case actually starves it. */
    if (xTaskCreatePinnedToCore(lp_task, "lpprobe", 4096, NULL, 5, NULL, 0)
        != pdPASS) {
        s_task_live = false;
        LPLOGE("could not start the trigger task");
    }
}
#else
static bool lp_task_running(void) { return false; }
static void lp_task_start(void) { }
#endif

void term_lp_probe_boot(void)
{
    if (!store_ready())
        return;

    uint8_t step = 0;
    if (!store_get_u8(LP_K_STEP, &step)) {
        /* Not armed. Re-log any results so a serial log alone is enough to
           read the outcome (MQTT is the primary path, this is the backup). */
        lp_verdict_t res[LP_RES_MAX];
        int n = store_get_res(res, LP_RES_MAX);
        if (n > 0) {
            LPLOGW("idle, %d verdict(s) from a completed sequence:", n);
            for (int i = 0; i < n; i++)
                lp_log_verdict(&res[i]);
        }
        return;
    }
    if (step >= TERM_LP_CAUSE_COUNT) {     /* corrupt state: refuse to guess */
        LPLOGE("state key out of range (%u); clearing", (unsigned)step);
        term_lp_probe_clear();
        return;
    }

    uint8_t pend = 0;
    store_get_u8(LP_K_PEND, &pend);
    if (pend) {
        uint32_t seq = 0;
        store_get_u32(LP_K_SEQ, &seq);
        lp_verdict_t v;
        memset(&v, 0, sizeof v);
        v.version = LP_RES_VERSION;
        v.expected = step;
        v.observed = (uint8_t)lp_reset_reason();
        lp_check(seq, &v);
        store_append_res(&v);
        lp_log_verdict(&v);

        step++;
        store_set_u8(LP_K_PEND, 0);
        if (step >= TERM_LP_CAUSE_COUNT) {
            lp_finish();
            return;
        }
        store_set_u8(LP_K_STEP, step);
        store_commit();
    }

    if (lp_stage(step))
        lp_task_start();
}

bool term_lp_probe_arm(void)
{
    if (!store_ready())
        return false;
    uint8_t step = 0;
    if (store_get_u8(LP_K_STEP, &step))
        return false;                      /* already running */
    lp_verdict_t res[LP_RES_MAX];
    if (store_get_res(res, LP_RES_MAX) > 0)
        return false;                      /* results present: clear first */
    if (!lp_stage(0))
        return false;
    lp_task_start();
    return true;
}

bool term_lp_probe_clear(void)
{
    if (!store_ready())
        return false;
    store_erase(LP_K_STEP);
    store_erase(LP_K_PEND);
    store_erase(LP_K_SEQ);
    store_erase(LP_K_RES);
    bool ok = store_commit();
    LPLOGW("state and results cleared (%s)", ok ? "committed" : "commit FAILED");
    return ok;
}

/* ------------------------------------------------------------------ */
/* report                                                              */
/* ------------------------------------------------------------------ */

static void lp_add(char *out, size_t cap, size_t *len, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

static void lp_add(char *out, size_t cap, size_t *len, const char *fmt, ...)
{
    if (*len >= cap)
        return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out + *len, cap - *len, fmt, ap);
    va_end(ap);
    if (n < 0) {
        *len = cap;                 /* poison: the caller sees truncation */
        return;
    }
    if ((size_t)n >= cap - *len)
        *len = cap;
    else
        *len += (size_t)n;
}

size_t term_lp_probe_report(char *out, size_t out_size)
{
    if (!out || out_size < 64)
        return 0;

    bool ready = store_ready();
    uint8_t step = 0, pend = 0;
    bool armed = false;
    int n = 0;
    lp_verdict_t res[LP_RES_MAX];

    if (ready) {
        armed = store_get_u8(LP_K_STEP, &step);
        store_get_u8(LP_K_PEND, &pend);
        n = store_get_res(res, LP_RES_MAX);
    }

    const char *state = "idle";
    if (armed)
        state = lp_task_running() ? "running" : "armed";
    else if (n > 0)
        state = "done";

    size_t len = 0;
    lp_add(out, out_size, &len,
           "{\"state\":\"%s\",\"supported\":%d,\"region\":%u,"
           "\"addr\":\"0x%08x\",\"delay_s\":%d,\"step\":%d,\"pend\":%d,"
           "\"causes\":[",
           state, ready ? 1 : 0, (unsigned)TERM_LP_PROBE_REGION_BYTES,
           (unsigned)(uintptr_t)&s_lp, TERM_LP_PROBE_DELAY_S,
           armed ? (int)step : -1, pend ? 1 : 0);
    for (int i = 0; i < TERM_LP_CAUSE_COUNT; i++)
        lp_add(out, out_size, &len, "%s\"%s\"", i ? "," : "",
               lp_cause_name((unsigned)i));
    /* Brownout is not software-triggerable; §11 item 3's degraded contract
       ("その cause では lastboot 無し") is what covers it. */
    lp_add(out, out_size, &len, "],\"untested\":[\"brownout\"],\"results\":[");
    for (int i = 0; i < n; i++) {
        const lp_verdict_t *v = &res[i];
        lp_add(out, out_size, &len,
               "%s{\"i\":%d,\"exp\":\"%s\",\"obs\":\"%s\",\"obs_id\":%u,"
               "\"magic\":%d,\"len\":%d,\"hdr\":%d,\"crc\":%d,\"noreset\":%d,"
               "\"seq_exp\":%u,\"seq_seen\":%u,\"match\":%u,\"bad\":%d,"
               "\"nz\":%u,\"of\":%u}",
               i ? "," : "", (int)v->expected, lp_cause_name(v->expected),
               lp_reason_name(v->observed), (unsigned)v->observed,
               (v->flags & LP_F_MAGIC) ? 1 : 0, (v->flags & LP_F_LEN) ? 1 : 0,
               (v->flags & LP_F_HDR) ? 1 : 0, (v->flags & LP_F_CRC) ? 1 : 0,
               (v->flags & LP_F_NORESET) ? 1 : 0,
               (unsigned)v->seq_expected, (unsigned)v->seq_seen,
               (unsigned)v->match_bytes, (int)v->first_bad,
               (unsigned)v->nonzero_bytes,
               (unsigned)TERM_LP_PROBE_PATTERN_BYTES);
    }
    lp_add(out, out_size, &len, "]}");

    if (len >= out_size) {                 /* truncated: say so, do not lie */
        snprintf(out, out_size, "{\"state\":\"%s\",\"error\":\"truncated\"}",
                 state);
        return strlen(out);
    }
    return len;
}
