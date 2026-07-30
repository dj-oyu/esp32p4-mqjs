/*
 * term_lp_probe.h — LP SRAM retention probe (docs/term-design.md §11 item 3).
 *
 * §4.4 builds the black-box flight recorder on one assumption: an
 * `RTC_NOINIT_ATTR` region in LP SRAM survives a panic, a watchdog reset and
 * a software reset with its contents intact. §11 item 3 refuses to implement
 * the ring until that assumption is measured on the device ("未検証のまま
 * 実装しない"), because ESP32-family parts have historically wiped the whole
 * RTC domain on some reset causes.
 *
 * This is that measurement: a 31 KiB `RTC_NOINIT_ATTR` payload (magic, seq,
 * CRC32, PRNG pattern) and an autonomous sequencer that crashes the device
 * once per reset cause and records, after each reboot, how much of the region
 * came back.
 *
 * Two properties that matter more than the numbers:
 *
 *   - **Inert by default.** The region is linked unconditionally (that is
 *     part of what is being tested — that the linker can place 31 KiB there),
 *     but nothing runs until `term_lp_probe_arm()` writes the NVS state key.
 *     A firmware with this file in it behaves exactly as before.
 *   - **The sequencer state lives in NVS, never in the region under test.**
 *     The region may be wiped by the very cause being measured, so it cannot
 *     also be the thing that remembers which cause is next. Every state
 *     advance is `nvs_commit`ed *before* the crash it enables.
 *
 * Safety: each cause is triggered `TERM_LP_PROBE_DELAY_S` seconds after boot,
 * so there is always a window in which a human can reflash out of a bug, and
 * `term_lp_probe_clear()` inside that window stands the sequence down. The
 * watchdog causes spin for a bounded time and record a "no reset observed"
 * verdict rather than hanging the device forever.
 *
 * Brownout (§11 item 3's fifth cause) is **not** covered: it cannot be
 * triggered from software. It stays untested, which the design's own fallback
 * already handles — "消える cause があれば『その cause では lastboot 無し』と
 * 契約に明記して縮退".
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The region under test. §4.4 budgets "LP SRAM 32KB: {header} + ~31KB ring",
   so the probe claims exactly 31 KiB — the same order the ring will ask for,
   which is the only size whose placement is worth proving. LP RAM on the P4
   is 32 KiB total; ~150 bytes are already spoken for by IDF's own .rtc.*
   sections, so the remainder after this region is a few hundred bytes. */
#define TERM_LP_PROBE_REGION_BYTES  (31u * 1024u)
#define TERM_LP_PROBE_HEADER_BYTES  32u
#define TERM_LP_PROBE_PATTERN_BYTES \
    (TERM_LP_PROBE_REGION_BYTES - TERM_LP_PROBE_HEADER_BYTES)

/* Seconds between a stage becoming live and the crash it triggers. This is a
   safety invariant, not a tuning knob: it is the window in which a bad build
   can be reflashed and in which term_lp_probe_clear() can stand the sequence
   down. Do not shorten it. */
#define TERM_LP_PROBE_DELAY_S       15

/* The reset causes, in the order the sequencer walks them. The index is
   persisted in NVS and appears in the report as `i`/`exp`, so appending is
   safe and reordering invalidates results collected earlier. */
typedef enum {
    TERM_LP_CAUSE_PANIC = 0,   /* volatile read of NULL -> LoadProhibited */
    TERM_LP_CAUSE_TASK_WDT,    /* subscribe + starve; bounded spin */
    TERM_LP_CAUSE_INT_WDT,     /* portDISABLE_INTERRUPTS + spin */
    TERM_LP_CAUSE_RESTART,     /* esp_restart() */
    TERM_LP_CAUSE_COUNT
} term_lp_cause_t;

/* Upper bound on the JSON `term_lp_probe_report` produces, including the
   NUL. Four verdicts plus the fixed preamble fit inside 1 KiB; the slack is
   there so an appended cause does not silently truncate the report. */
#define TERM_LP_PROBE_REPORT_MAX    2048

/*
 * Boot entry point. Call once, early, from app_main (it opens NVS itself and
 * needs nothing else initialised).
 *
 * Does nothing at all unless a sequence is armed. When one is:
 *   1. if the previous boot fired a cause, judge the region against the seq
 *      that was persisted before the crash and append the verdict to NVS;
 *   2. re-fill the region with a fresh pattern for the next cause and commit
 *      the new state;
 *   3. start the trigger task (delay, then crash).
 * After the last cause the state keys are erased and the results stay in NVS
 * until read (`term_lp_probe_report`) and explicitly cleared.
 */
void term_lp_probe_boot(void);

/*
 * Arm the sequence. One-shot and convergent, because the dev-slot script that
 * calls it re-runs on every boot: returns false and changes nothing if a
 * sequence is already in progress *or* if results are already present.
 * Clearing is the only way to run a second sequence.
 *
 * Returns true when this call is the one that armed it. Always false off the
 * device.
 */
bool term_lp_probe_arm(void);

/*
 * Erase the sequencer state and every collected verdict. Also stands down a
 * sequence that is inside its pre-crash delay window (the trigger task
 * re-reads the state after the delay and exits when it is gone), which makes
 * this the abort button for a probe armed by mistake.
 */
bool term_lp_probe_clear(void);

/*
 * Render the state and the verdicts as JSON into `out`, NUL-terminated.
 * Returns the number of bytes written excluding the NUL, or 0 if `out` is
 * NULL or `out_size` is too small to hold even the shortest report.
 *
 *   {"state":"idle"|"armed"|"running"|"done",
 *    "supported":1, "region":31744, "addr":"0x50108080", "delay_s":15,
 *    "step":2, "pend":0,
 *    "causes":["panic","task_wdt","int_wdt","restart"],
 *    "untested":["brownout"],
 *    "results":[{"i":0,"exp":"panic","obs":"panic","obs_id":4,
 *                "magic":1,"len":1,"hdr":1,"crc":1,"noreset":0,
 *                "seq_exp":305419896,"seq_seen":305419896,
 *                "match":31712,"bad":-1,"nz":31694}]}
 *
 * Per verdict: `exp` is the cause the sequencer intended, `obs`/`obs_id` the
 * `esp_reset_reason()` the following boot actually saw (they disagree when the
 * cause resets for a different documented reason, and that disagreement is
 * itself a finding). `magic`/`len`/`hdr`/`crc` are the four header/body checks
 * as separate bits, so partial survival is distinguishable from full survival:
 * `match` counts bytes of the pattern that equal the expected PRNG stream,
 * `bad` is the first mismatching offset (-1 when none) and `nz` counts
 * non-zero bytes, which separates "wiped to zeros" from "overwritten with
 * something". When `noreset` is 1 the cause did not reset the device inside
 * its bounded spin and the survival fields carry no information.
 */
size_t term_lp_probe_report(char *out, size_t out_size);

#ifdef __cplusplus
}
#endif
