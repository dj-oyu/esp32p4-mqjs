/*
 * term_lp_ring — the LP SRAM black box (flight recorder).
 *
 * docs/term-design.md §4.4, phase 3 of §11. LP SRAM is 32 KiB that the HP
 * core reaches over a slow bus but which SURVIVES panic, watchdog reset and
 * software reset (`RTC_NOINIT_ATTR`; not a power cut). PHASE3_MANIFEST.md §1
 * is the on-device measurement that licenses this file: panic, interrupt WDT
 * and `esp_restart` each returned 31,712/31,712 bytes bit-perfect, so a
 * region-level check suffices and no per-record CRC is needed for
 * correctness. Brownout is untested and assumed to wipe (§4.4: "LP は電源断
 * で消える"); a task WDT never resets this firmware at all
 * (CONFIG_ESP_TASK_WDT_PANIC unset), so it also never produces a `lastboot`.
 *
 * What this gives the operator: after a crash, the last few hundred log lines
 * that the device emitted before it died, tagged with who wrote them, pullable
 * over MQTT behind the Ed25519 gate of §7.2 (the responder is the next
 * dispatch and is a pure CONSUMER of the read API below — it adds no state
 * here).
 *
 * =====================================================================
 * THE SIX PROPERTIES
 * =====================================================================
 *
 * P1. TWO STATIC PARTITIONS, NO DYNAMIC QUOTAS (§4.4). System 8 KiB, app
 *     23 KiB, fixed at compile time. The threat is eviction DoS: with one
 *     shared ring a chatty or malicious app's spam flushes the platform's
 *     last words. A static split makes that structurally impossible, and
 *     §4.4 explicitly rejects per-writer dynamic quotas as not worth the
 *     complexity.
 *
 * P2. EVERY RECORD CARRIES {writer_id, class} STRUCTURALLY (§4.4). The
 *     writer is an index into a table of interned names in the region
 *     header, so a 40-byte log line does not pay 32 bytes of provenance,
 *     and the name survives the eviction of the record that first
 *     introduced it. NOTHING is recovered by parsing a prefix string out of
 *     the text.
 *
 * P3. CLASS B IS UNREACHABLE, NOT FILTERED (§3.2, §7.1). The only way into
 *     this ring is term_registry's LINE-ORIENTED log path. `term.feed`, the
 *     byte ring, the VT parser and the ssh pipe never call append — there is
 *     no code path from SSH session content to this memory, so no filter can
 *     be wrong about it. Preedit is likewise absent because it never enters
 *     term state at all (§10.2).
 *
 * P4. THE REGION ONLY EVER HOLDS STRIPPED TEXT. term_lp_ring_append() runs
 *     term_lp_strip() on every byte it accepts; there is no API that writes
 *     raw bytes. See "WHERE SGR STRIPPING HAPPENS" below.
 *
 * P5. A CRASH DURING AN APPEND COSTS AT MOST THE RECORD IN FLIGHT. The
 *     region header is double-buffered and every content byte is written
 *     into space the currently-valid header calls free. A reader takes the
 *     valid header slot with the highest `hseq`, and the content is always
 *     consistent with it. This matters more here than anywhere else in the
 *     project: the append most likely to be interrupted is the one
 *     immediately before the crash, i.e. exactly the line the operator
 *     wants to read.
 *
 * P6. NO ALLOCATION ON THE APPEND PATH, AND NO LP READS ON THE HOT PATH
 *     BEYOND EVICTION. The header lives in a DRAM shadow; appends are
 *     sequential, 8-byte-aligned writes. The one LP read is the 8-byte
 *     header of a record being evicted from the tail (§4.4 asks for
 *     sequential writes and no random access; ~8 bytes read per ~70 written
 *     is the whole of it).
 *
 * =====================================================================
 * REGION LAYOUT (the on-memory format — this IS the contract)
 * =====================================================================
 *
 *   offset   size    contents
 *   0        128     term_lp_hdr_t, slot A
 *   128      128     term_lp_hdr_t, slot B          (P5: double buffer)
 *   256      256     writer table: 8 x 32-byte NUL-padded names
 *   512      8192    SYS partition data (§4.4: platform events, panic
 *                    reasons, boot markers, system apps)
 *   8704     23552   APP partition data (§4.4: user apps' print/term.log)
 *   -------------------
 *   32256            TERM_LP_REGION_BYTES
 *
 * A partition is a circular buffer of records. One record is an 8-byte
 * term_lp_rec_t followed by `len` payload bytes, the whole thing rounded up
 * to TERM_LP_REC_ALIGN. Records never straddle the end of a partition: a
 * TERM_LP_CLASS_PAD record fills the remainder and the next record starts at
 * offset 0. The reader therefore never has to reassemble a split record, and
 * the writer never issues an unaligned or wrapped burst.
 *
 * =====================================================================
 * WHERE SGR STRIPPING HAPPENS (§12's open question, resolved)
 * =====================================================================
 *
 * §12 guessed that "after attribute run-splitting at ingest the plain text
 * should be at hand". IT IS NOT, in this implementation: ingest (§5) copies
 * raw bytes into a per-term byte ring and the parser runs LATER, on the UI
 * frame task, producing attributed CELLS rather than runs of text. Stripping
 * there would have been wrong three times over:
 *
 *   - it would have to re-serialise cells back into text, undoing the
 *     parser's work and losing the original line grouping;
 *   - it runs on the UI task, one frame or more after the writer spoke — so
 *     a device that dies before the next frame would lose precisely the last
 *     words the black box exists to keep;
 *   - the parser sees class B (SSH) bytes too, so the class separation would
 *     become a filter instead of a structure (P3).
 *
 * So the strip happens IN term_lp_ring_append(), at the line-oriented ingest
 * seam, synchronously with the writer. term_lp_strip() is the whole rule and
 * is exposed here because it is contract, not detail: it drops every
 * ESC-introduced sequence (CSI/OSC/DCS/SS2/SS3/single-character), drops the
 * remaining C0 controls except '\n' and '\t', and passes everything >= 0x20
 * through byte for byte (so UTF-8 is preserved untouched).
 *
 * Dropping ALL escapes rather than only SGR is deliberate and is a security
 * property, not tidiness: the black box is pulled off the device and printed
 * on the operator's terminal, so a retained escape sequence would let any app
 * that can call print() inject terminal control into the reader's session.
 *
 * =====================================================================
 * LASTBOOT: HOW THE PREVIOUS SESSION IS FROZEN
 * =====================================================================
 *
 * §4.4 requires that the magic+CRC-validated previous contents stay readable
 * ("再起動後 ... 前ブート分を署名付き pull で読める") while the new session
 * starts logging immediately. term_lp_ring_boot() therefore, in this order:
 *
 *   1. validates the retained image (term_lp_check_t);
 *   2. if it is valid AND holds records, COPIES THE WHOLE REGION IMAGE to
 *      PSRAM and marks it read-only. That snapshot is `lastboot`;
 *   3. re-formats the LP region for this session (boot_seq = previous + 1);
 *   4. appends the boot marker to the SYS partition.
 *
 * Copy-out beats sealing in place. LP SRAM is the scarce resource — 32 KiB
 * total, no second source — and sealing would permanently halve the live
 * ring, i.e. spend the irreplaceable memory to save the abundant kind
 * (§4.1's budget is 1.3 MB of PSRAM for eight terminals; 31 KiB more is
 * 2.4% of that and 0.1% of the chip's PSRAM). The copy is also allocated
 * ONLY when there is something to freeze, so a cold power-on pays nothing,
 * and it makes the frozen image bit-identical in layout to the live one —
 * which is why one iterator serves both sources and the MQTT responder needs
 * no second decoder.
 *
 * =====================================================================
 * WIPED vs EMPTY, AND WHY THIS SUPERSEDES THE RETENTION PROBE
 * =====================================================================
 *
 * The boot marker is written on EVERY boot, and it records this boot's
 * `esp_reset_reason()` next to the verdict on the previous contents. Those
 * two together are what tell a reader which of three worlds it is in:
 *
 *   reset reason      previous image     meaning
 *   poweron/brownout  absent (zeros)     COLD: expected, LP lost its charge
 *   panic/int_wdt/sw  valid              OK: retention working as measured
 *   panic/int_wdt/sw  absent (zeros)     LOST: a retention failure, and a
 *                                        finding worth reporting
 *   any               present, invalid   CORRUPT: something else wrote here
 *
 * That is a permanent, always-on version of what PHASE3_MANIFEST.md §1's
 * throwaway probe measured once, on real data instead of a PRNG pattern —
 * including the two causes the probe could not reach (brownout, and a task
 * WDT if CONFIG_ESP_TASK_WDT_PANIC is ever turned on). The probe has been
 * removed rather than left to claim the same LP region from a second owner;
 * it is recoverable from commit 77b1c99 if a dedicated crash sequencer is
 * ever wanted again. term_lp_stats_t::retention is this verdict.
 *
 * =====================================================================
 * SERIALISATION (read this before calling append from anywhere new)
 * =====================================================================
 *
 * THIS MODULE CONTAINS NO LOCK, ON PURPOSE. Mutating calls
 * (term_lp_ring_format / _append, term_lp_log) must be serialised by the
 * caller. On the device that serialisation is term_registry's single table
 * mutex, which the tee already holds; the lock order is registry -> ring and
 * the ring never calls back, so no second order exists. term_lp_ring_boot()
 * runs as the first statement of app_main, before any other task can reach
 * the ring. Adding a second mutex here would buy nothing and would create
 * exactly the nested-lock question this project has already paid for once.
 *
 * Read calls (term_lp_iter_*, term_lp_report, term_lp_dump_json) do not
 * mutate. Against the LASTBOOT source they are entirely race-free: the
 * snapshot is immutable. Against LIVE they may observe a torn tail if an
 * append is running concurrently, so a caller that wants a coherent live
 * dump should hold the same lock the appends do.
 *
 * This header is C99 and includes nothing but <stdint.h>/<stddef.h>/
 * <stdbool.h> — no ESP-IDF, no FreeRTOS, no term_registry. Same rule as
 * term_core.h, and the reason the ring's logic is host-testable against a
 * plain malloc'd region.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===================================================================== */
/* Layout constants                                                      */
/* ===================================================================== */

/* "TLR1", most significant byte first. A region whose first word is not
 * this has never been formatted by this layout version. */
#define TERM_LP_MAGIC        0x544C5231u
#define TERM_LP_VERSION      1u

#define TERM_LP_HDR_BYTES    128u   /* sizeof(term_lp_hdr_t), asserted     */
#define TERM_LP_HDR_SLOTS    2u     /* P5: double-buffered header          */

/* Writer table (P2). Names are stored whole, never truncated: two distinct
 * apps must not be able to present the same writer_id, which is the same
 * argument that made term_registry's owner field 32 bytes. When the table is
 * full a record gets TERM_LP_WRITER_UNKNOWN rather than somebody else's
 * identity, and term_lp_stats_t::unnamed counts how often that happened.
 *
 * A name that CANNOT be stored whole — 32 bytes or longer, so it would have
 * to be cut to fit an entry — takes exactly the same path: WRITER_UNKNOWN
 * plus one on `unnamed`, never a truncated entry. Real writers are bounded at
 * 31 bytes by MQJS_APP_NAME_MAX, so this is defence in depth, but it is what
 * makes the sentence above true without a qualifier. */
#define TERM_LP_WRITERS      8u
#define TERM_LP_WRITER_MAX   32u    /* == MQJS_APP_NAME_MAX                */
#define TERM_LP_WT_BYTES     (TERM_LP_WRITERS * TERM_LP_WRITER_MAX)
#define TERM_LP_WRITER_UNKNOWN 0xFFu

/* The name platform code writes under (the console, boot markers, panic
 * reasons). term_registry maps its own TERM_OWNER_SYSTEM ("\1system", which
 * carries a control byte no app name may contain) onto this. */
#define TERM_LP_WRITER_SYSTEM "system"

/* §4.4's static split. Both are multiples of TERM_LP_REC_ALIGN.
 *
 * If the device link ever objects to the region size (LP RAM is 32,744 B and
 * IDF's own .rtc.* sections take ~150 B, leaving ~336 B of slack after this
 * region), TERM_LP_APP_BYTES is the ONE number to reduce. The linker fails
 * the build rather than relocating the region, so this cannot go wrong
 * silently — see PHASE3_MANIFEST.md §1 for the linker-script evidence. */
#define TERM_LP_SYS_BYTES    8192u
#define TERM_LP_APP_BYTES    23552u

#define TERM_LP_WT_OFF       (TERM_LP_HDR_BYTES * TERM_LP_HDR_SLOTS)
#define TERM_LP_SYS_OFF      (TERM_LP_WT_OFF + TERM_LP_WT_BYTES)
#define TERM_LP_APP_OFF      (TERM_LP_SYS_OFF + TERM_LP_SYS_BYTES)
#define TERM_LP_REGION_BYTES (TERM_LP_APP_OFF + TERM_LP_APP_BYTES)  /* 32256 */

/* Every record header, payload and partition size is a multiple of this, so
 * (a) every LP write is 8-byte aligned and (b) the remainder before the end
 * of a partition can always hold a PAD header. */
#define TERM_LP_REC_ALIGN    8u
#define TERM_LP_REC_HDR      8u     /* sizeof(term_lp_rec_t), asserted     */

/* Longest payload one record can hold. A longer line is written truncated,
 * back off to a UTF-8 boundary, with TERM_LP_F_TRUNC set — never split
 * across two records, because half a line attributed to the same writer is
 * indistinguishable from two lines. */
#define TERM_LP_REC_MAX      512u

/* Buffer sizes for the two JSON builders. */
#define TERM_LP_REPORT_MAX   1024u
#define TERM_LP_DUMP_MAX     4096u

/* ===================================================================== */
/* Classes, partitions, sources                                          */
/* ===================================================================== */

/*
 * The `class` half of §4.4's record tag, and the partition selector. Both
 * SYS and APP are class A data (§7.1) — the distinction here is the
 * anti-eviction-DoS split of P1, not a confidentiality boundary. §4.4 is
 * explicit that system and user logs mix on purpose ("クラッシュ調査で一番
 * 読みたいのはハングしたユーザーアプリの直前ログ").
 */
typedef enum {
    TERM_LP_CLASS_APP = 0,  /* user app print()/term.log -> APP partition  */
    TERM_LP_CLASS_SYS = 1,  /* platform + system apps     -> SYS partition */
    TERM_LP_CLASS_PAD = 15, /* filler to a partition end; never content    */
} term_lp_class_t;

/* Record flags, carried in the high nibble of term_lp_rec_t::cls. */
#define TERM_LP_F_TRUNC 0x10u   /* payload was cut at TERM_LP_REC_MAX */

#define TERM_LP_CLASS_OF(c) ((term_lp_class_t)((unsigned)(c) & 0x0Fu))
#define TERM_LP_FLAGS_OF(c) ((unsigned)(c) & 0xF0u)

typedef enum {
    TERM_LP_PART_SYS = 0,
    TERM_LP_PART_APP = 1,
    TERM_LP_PART_COUNT = 2,
} term_lp_part_id_t;

/* Which image a read is against. */
typedef enum {
    TERM_LP_SRC_LIVE = 0,      /* this session's ring, still being written */
    TERM_LP_SRC_LASTBOOT = 1,  /* the frozen previous session, or absent   */
} term_lp_src_t;

/* ===================================================================== */
/* On-memory structures                                                  */
/* ===================================================================== */

/* One record header. 8 bytes, all fields naturally aligned.
 *
 * `t_ms` is milliseconds since THIS boot (the port's monotonic clock), not
 * wall time: the black box is read after a reset, when the only honest
 * statement about a record is how long after boot it was written. It wraps
 * after 49 days of uptime; a reader that sees times going backwards inside
 * one boot is looking at a wrap, not at corruption. */
typedef struct {
    uint16_t len;     /* payload bytes that follow; 0 <= len <= REC_MAX    */
    uint8_t  writer;  /* index into the writer table, or WRITER_UNKNOWN    */
    uint8_t  cls;     /* term_lp_class_t | TERM_LP_F_* (use the macros)    */
    uint32_t t_ms;    /* ms since boot at append time                      */
} term_lp_rec_t;

/* One partition descriptor, 32 bytes. `used` is carried explicitly because
 * head == tail is otherwise ambiguous between empty and full. */
typedef struct {
    uint32_t off;       /* data area offset within the region (constant)   */
    uint32_t cap;       /* data area size (constant, multiple of ALIGN)    */
    uint32_t head;      /* next write offset, [0, cap)                     */
    uint32_t tail;      /* oldest live record, [0, cap)                    */
    uint32_t used;      /* live bytes including PAD records, [0, cap]      */
    uint32_t records;   /* live content records (PAD not counted)          */
    uint32_t appended;  /* content records ever appended this boot         */
    uint32_t evicted;   /* content records dropped from the tail this boot */
} term_lp_part_t;

/*
 * The region header, 128 bytes, stored twice (P5). A reader takes the slot
 * that validates with the greater `hseq`; a writer always writes the OTHER
 * slot, so a torn header write can never destroy the only good copy.
 *
 * `crc` covers bytes [0, offsetof(crc)) of this struct — the design's
 * "{magic, boot_seq, head, crc}" applied to the metadata that has to be
 * right for the region to be walkable at all. It is recomputed on every
 * append, which costs 128 bytes of hashing over a DRAM shadow, not a pass
 * over 31 KiB of LP SRAM. Payload bytes are NOT covered: PHASE3_MANIFEST §1
 * measured retention as bit-perfect or absent, never subtly rotten, so a
 * per-record CRC would buy nothing that the structural chain walk
 * (term_lp_check_t::chain_ok) does not already catch.
 */
typedef struct {
    uint32_t magic;          /* TERM_LP_MAGIC                              */
    uint16_t version;        /* TERM_LP_VERSION                            */
    uint16_t hdr_bytes;      /* TERM_LP_HDR_BYTES, for a future decoder    */
    uint32_t region_bytes;   /* TERM_LP_REGION_BYTES                       */
    uint32_t hseq;           /* ++ per header write; picks the live slot   */
    uint32_t boot_seq;       /* ++ per boot; 1 after a wipe (the marker)   */
    uint32_t prev_boot_seq;  /* boot_seq of the image captured at boot      */
    uint8_t  reset_reason;   /* esp_reset_reason() of the boot that formatted */
    uint8_t  prev;           /* term_lp_prev_t: what boot found here       */
    uint8_t  writers;        /* interned names in the writer table         */
    uint8_t  rsv8;
    uint32_t wt_crc;         /* CRC32 over the first `writers` names       */
    uint32_t dropped_bytes;  /* payload bytes lost to eviction, both parts */
    uint32_t truncated;      /* records cut at TERM_LP_REC_MAX             */
    uint32_t refused;        /* appends this ring rejected: empty after
                              * stripping, or a class no partition holds   */
    uint32_t unnamed;        /* appends that got WRITER_UNKNOWN            */
    term_lp_part_t part[TERM_LP_PART_COUNT];
    uint32_t rsv[3];
    uint32_t crc;            /* CRC32 over every byte above                */
} term_lp_hdr_t;

/* ===================================================================== */
/* Validation                                                            */
/* ===================================================================== */

/*
 * What term_lp_ring_open() found. Every field is separately meaningful: a
 * wipe and a stray write look nothing alike and §4.4 cares which one it is
 * facing (this is the same discipline the retention probe used).
 *
 * WHERE THE BOUNDARY BETWEEN geometry_ok AND chain_ok RUNS. Both describe
 * "the region is walkable", but they read different bytes, and a reader that
 * has to explain a failure wants to know which:
 *
 *   geometry_ok is decided from the HEADER ALONE. Each partition descriptor's
 *   off/cap must equal the compile-time constants; head, tail and used must be
 *   in range and 8-byte aligned; `writers` must be <= TERM_LP_WRITERS; and
 *   `used` must agree with the ring arithmetic, (tail + used) % cap == head.
 *   That last one is why a `used` that contradicts head/tail is a GEOMETRY
 *   failure and not a chain failure — it is caught before a single record byte
 *   is read, and with the count wrong there is no walk worth attempting.
 *
 *   chain_ok is decided by the RECORD WALK, and covers only what the walk can
 *   see: each record's class is APP/SYS/PAD, each `len` is within its cap, no
 *   record straddles a partition end, the chain from `tail` consumes exactly
 *   `used` bytes and lands on `head`, and the content-record count matches the
 *   descriptor's `records`. A smashed record header mid-chain therefore shows
 *   up as chain_ok=false with crc_ok and geometry_ok still true.
 *
 * chain_ok is only computed when geometry_ok holds (there is nothing to walk
 * otherwise), so read the two in that order.
 */
typedef struct {
    bool magic_ok;    /* a formatted region of a recognised layout          */
    bool version_ok;
    bool crc_ok;      /* a header slot validated                            */
    bool geometry_ok; /* header self-consistent: descriptors, ranges,
                       * alignment, and used == head-tail arithmetic        */
    bool chain_ok;    /* both record chains walk from tail to head exactly;
                       * false only when geometry_ok is true                */
    bool wt_crc_ok;   /* writer names intact (false only loses NAMES)       */
    bool zeroed;      /* !magic_ok and every byte of the region is 0        */
    bool ok;          /* magic && version && crc && geometry && chain       */

    uint8_t  slot;            /* header slot used, 0..TERM_LP_HDR_SLOTS-1  */
    uint32_t boot_seq;
    uint8_t  reset_reason;    /* of the session that wrote this image      */
    uint32_t records[TERM_LP_PART_COUNT];
    uint32_t used[TERM_LP_PART_COUNT];
} term_lp_check_t;

/* What boot found in the region. Reported by term_lp_stats_t::prev and in
 * the boot marker text. */
typedef enum {
    TERM_LP_PREV_NONE     = 0, /* no magic, region all zeros: wiped or new  */
    TERM_LP_PREV_CAPTURED = 1, /* valid and non-empty: frozen as lastboot   */
    TERM_LP_PREV_EMPTY    = 2, /* valid but logged nothing                  */
    TERM_LP_PREV_NOMEM    = 3, /* valid, but the snapshot alloc failed      */
    TERM_LP_PREV_BAD      = 4, /* magic present, image did not validate     */
    TERM_LP_PREV_GARBAGE  = 5, /* no magic but non-zero: a foreign writer   */
} term_lp_prev_t;

/* The standing retention verdict (see "WIPED vs EMPTY" above). */
typedef enum {
    TERM_LP_RET_OK      = 0, /* previous image survived this reset          */
    TERM_LP_RET_COLD    = 1, /* nothing there, and the reset explains it    */
    TERM_LP_RET_LOST    = 2, /* nothing there after a reset that should
                              * have retained: a finding                    */
    TERM_LP_RET_CORRUPT = 3, /* something was there and did not validate    */
    TERM_LP_RET_UNKNOWN = 4, /* no region at all: the initial value, before
                              * boot has judged anything. NOT the host build —
                              * off-device the region is ordinary .bss, so a
                              * host/run_pc boot judges it like any other and
                              * normally reports RET_COLD.                  */
} term_lp_retention_t;

/* ===================================================================== */
/* The ring handle                                                       */
/* ===================================================================== */

/*
 * Small enough to live on a test's stack. The header shadow is the
 * authoritative copy while the ring is writable: the LP region is written
 * through, never read back on the append path (P6).
 */
typedef struct {
    uint8_t      *base;      /* the region, 8-byte aligned                 */
    size_t        bytes;     /* >= TERM_LP_REGION_BYTES                    */
    bool          attached;
    bool          read_only; /* a frozen snapshot: append always refuses   */
    bool          wt_ok;     /* writer names validated; else names read "?" */
    uint8_t       slot;      /* header slot last written                   */
    /* One-entry writer cache, so the usual run of records from the same
     * writer resolves its index without touching the region at all. */
    uint8_t       wcache_idx;
    char          wcache[TERM_LP_WRITER_MAX];
    term_lp_hdr_t hdr;       /* DRAM shadow                                */
} term_lp_ring_t;

/* ===================================================================== */
/* Pure core (host-testable against any malloc'd region)                 */
/* ===================================================================== */

/*
 * Format `region` as an empty ring and attach `r` to it. `bytes` must be at
 * least TERM_LP_REGION_BYTES and `region` 8-byte aligned; anything else
 * returns false and leaves both untouched.
 *
 * Leaves exactly ONE published slot: slot 1 is zeroed and slot 0 is written
 * with hseq = 1. That is what keeps a leftover header from an earlier session
 * — whose hseq may be far higher than the new one — from winning the
 * "greatest valid hseq" race; zeroing it removes it from the race entirely
 * rather than trying to outrank it.
 *
 * `boot_seq` and `reset_reason` are recorded for the reader; `prev` and
 * `prev_boot_seq` describe what the caller found before formatting, and are
 * what the boot marker quotes.
 */
bool term_lp_ring_format(term_lp_ring_t *r, void *region, size_t bytes,
                         uint32_t boot_seq, uint8_t reset_reason,
                         term_lp_prev_t prev, uint32_t prev_boot_seq);

/*
 * Attach to an existing region and validate it. `out` (may be NULL) receives
 * the full verdict. Returns true only when `out->ok` — i.e. when the header
 * validated and both record chains walked cleanly — in which case `r` is
 * usable for reading, and for writing too unless `read_only`.
 *
 * A false return leaves `r` unattached: a region that did not validate is
 * never appended to, it is re-formatted.
 */
bool term_lp_ring_open(term_lp_ring_t *r, void *region, size_t bytes,
                       bool read_only, term_lp_check_t *out);

/*
 * Append one record. `cls` selects the partition (P1), `writer` is interned
 * into the table (P2; NULL or empty means WRITER_UNKNOWN), `text`/`len` is
 * the payload BEFORE stripping (P4) and `t_ms` the monotonic timestamp.
 *
 * Lossy at the tail, never at the head: records are evicted from the oldest
 * end until the new one fits, which is what a flight recorder is. Returns
 * false without changing anything when the ring is not attached, is
 * read-only, `cls` is neither APP nor SYS, or the text is empty after
 * stripping.
 *
 * WHAT `refused` COUNTS is narrower than that list: only the refusals that
 * happen on a LIVE ATTACHED ring, i.e. an empty-after-stripping text or a
 * `cls` the partitions cannot hold. The other two cannot be counted and must
 * not be:
 *
 *   - an unattached handle has no header to count in at all;
 *   - a read-only handle refuses WITHOUT counting, because its header is a
 *     LASTBOOT snapshot's header. Bumping a counter there would make the
 *     frozen image's own numbers drift away from the bytes they describe (and
 *     would make the CRC that guards them stale), so a snapshot's counters
 *     stay exactly as the session that wrote them left them.
 *
 * MUST be serialised by the caller — see "SERIALISATION" above.
 */
bool term_lp_ring_append(term_lp_ring_t *r, term_lp_class_t cls,
                         const char *writer, const char *text, size_t len,
                         uint32_t t_ms);

/*
 * The strip rule of P4, exposed because it is contract. Copies `src` into
 * `dst` (NULL to measure only), dropping:
 *
 *   - every ESC-introduced sequence, whole: CSI (ESC [ ... final byte
 *     0x40-0x7E), OSC/DCS/APC/PM (ESC ] P X ^ _ ... terminated by BEL or
 *     ESC \ or the end of the input), nF sequences (ESC + an intermediate
 *     0x20-0x2F, then bytes up to a final 0x30-0x7E — this is what makes
 *     `ESC ( B` vanish completely instead of leaving a stray "B"), and any
 *     other ESC + one byte;
 *   - the remaining C0 controls (0x00-0x1F) and DEL, except '\n' and '\t';
 *
 * and passing every byte >= 0x20 through unchanged, so UTF-8 sequences
 * survive byte for byte. An unterminated escape at the end of the input
 * consumes the rest of the input, which is the safe direction: a partial
 * sequence never leaks its bytes into the region.
 *
 * Returns the number of bytes written (or that would be). When the result
 * would exceed `dst_cap` the copy stops at the last complete UTF-8 sequence
 * that fits and *out_trunc (may be NULL) is set true — never mid-codepoint,
 * so a pulled log is always valid UTF-8.
 *
 * MEASURING IS CAPPED TOO. `dst_cap` bounds the answer whether or not `dst`
 * is NULL: term_lp_strip(NULL, 3, "hello", 5, &t) returns 3 and sets `t`,
 * and dst_cap 0 measures 0. So dst=NULL means "do not write", not "no limit"
 * — to measure the whole stripped length, pass a cap you know is big enough
 * (TERM_LP_REC_MAX, or `len`, which is always an upper bound because
 * stripping only ever removes bytes). The one difference from a real copy is
 * that a capped measurement does NOT back off to a codepoint boundary: with
 * no `dst` there are no bytes to inspect, so the count is the raw cap. A
 * caller that needs the exact stored length must therefore measure with a
 * cap that does not bite, or read the return value of the real copy.
 */
size_t term_lp_strip(char *dst, size_t dst_cap, const char *src, size_t len,
                     bool *out_trunc);

/* ===================================================================== */
/* Reading (the MQTT responder of §7.2 is a consumer of exactly this)     */
/* ===================================================================== */

/* One record, resolved. `text` points into the region/snapshot and is NOT
 * NUL-terminated; it stays valid as long as nobody appends (always, for a
 * LASTBOOT snapshot).
 *
 * `index` is the record's ordinal within its partition THIS BOOT, 0-based:
 * the first record ever appended to a partition is index 0. It counts
 * appends, not survivors, so it keeps rising across eviction — after the
 * first record is dropped the oldest one an iteration yields is index 1, and
 * the numbers stay dense and gap-free within one iteration. Two records in
 * different partitions can therefore share an index; it is a per-partition
 * ordinal, not a global one. */
typedef struct {
    term_lp_class_t cls;
    unsigned        flags;    /* TERM_LP_F_*                              */
    const char     *writer;   /* resolved name, never NULL ("?" if lost)  */
    uint32_t        t_ms;
    uint32_t        index;    /* 0-based ordinal in its partition (above) */
    const uint8_t  *text;
    uint16_t        len;
} term_lp_record_t;

/* Oldest first. Treat as opaque. */
typedef struct {
    const term_lp_ring_t *r;
    int      part;      /* partition being walked                          */
    int      part_end;  /* one past the last partition to walk             */
    uint32_t off;       /* offset within the current partition             */
    uint32_t left;      /* bytes left to consume in the current partition  */
    uint32_t index;     /* ordinal of the next content record             */
} term_lp_iter_t;

/*
 * Begin an iteration over `part` (TERM_LP_PART_SYS / _APP), or over both in
 * SYS-then-APP order when `part` is negative. Records come out oldest first;
 * PAD records are skipped and never seen by the caller.
 */
void term_lp_iter_begin(term_lp_iter_t *it, const term_lp_ring_t *r, int part);
bool term_lp_iter_next(term_lp_iter_t *it, term_lp_record_t *out);

/* ===================================================================== */
/* The device singleton                                                  */
/* ===================================================================== */

/*
 * Boot entry point. Call as the first statement of app_main, before anything
 * can log: it validates the retained region, freezes the previous session
 * (see LASTBOOT above), re-formats for this session and appends the boot
 * marker.
 *
 * IDEMPOTENT AND SELF-STARTING: a second call does nothing, and every other
 * entry point in this section calls it first, so a build that forgets the
 * boot hook still records — it simply gets a later timestamp on the boot
 * marker. Nothing can overwrite the retained image before this runs, because
 * this module is the only writer of the region.
 *
 * Off the device the region is ordinary .bss: everything works, nothing is
 * retained, and term_lp_stats_t::retained is false.
 */
void term_lp_ring_boot(void);

/* True once term_lp_ring_boot() has attached a live ring. */
bool term_lp_ring_ready(void);

/*
 * The tee (§4.4: "ingest がスクロールバックに書くついでに同じバイト列を
 * tee"). Appends to the live ring with the port's clock; the only writer
 * inside this component is term_registry's line-oriented log path (P3).
 * Returns false when there is no ring or the text was empty after
 * stripping. Serialisation: the caller's, as above.
 */
bool term_lp_log(term_lp_class_t cls, const char *writer,
                 const char *text, size_t len);

/* The two readable images. LASTBOOT is NULL when this boot captured none. */
const term_lp_ring_t *term_lp_source(term_lp_src_t src);

/* ===================================================================== */
/* Introspection                                                         */
/* ===================================================================== */

typedef struct {
    bool     ready;      /* a live ring is attached                        */
    bool     retained;   /* the region really is LP SRAM (device build)    */
    uint32_t region_bytes;
    uint32_t boot_seq;
    uint8_t  reset_reason;      /* this boot's esp_reset_reason()          */
    term_lp_prev_t      prev;   /* what this boot found                    */
    term_lp_retention_t retention;
    uint32_t prev_boot_seq;
    bool     lastboot;          /* a frozen snapshot exists                */

    /* Per partition, live then lastboot (zeroed when there is none). */
    uint32_t cap[TERM_LP_PART_COUNT];
    uint32_t used[TERM_LP_PART_COUNT];
    uint32_t records[TERM_LP_PART_COUNT];
    uint32_t appended[TERM_LP_PART_COUNT];
    uint32_t evicted[TERM_LP_PART_COUNT];
    uint32_t last_records[TERM_LP_PART_COUNT];
    uint32_t last_used[TERM_LP_PART_COUNT];

    uint32_t writers;        /* interned names                             */
    uint32_t dropped_bytes;  /* payload lost to eviction this boot         */
    uint32_t truncated;
    uint32_t refused;
    uint32_t unnamed;
    uint32_t hseq;
} term_lp_stats_t;

void term_lp_stats(term_lp_stats_t *out);

const char *term_lp_prev_str(term_lp_prev_t p);
const char *term_lp_retention_str(term_lp_retention_t v);
/* esp_reset_reason_t by value, spelled numerically so the table also
 * compiles off-device. */
const char *term_lp_reason_str(unsigned reason);

/*
 * Stats as JSON, NUL-terminated, no content and no writer NAMES — this is
 * the unprivileged half of the JS surface (§7.2: apps get no path to another
 * app's content; counters are telemetry, like sys.heap()).
 *
 *   {"ready":1,"retained":1,"region":32256,"boot_seq":7,
 *    "reset":"panic","reset_id":4,"retention":"ok","prev":"captured",
 *    "prev_boot_seq":6,"lastboot":1,"rec_max":512,"writers":3,
 *    "live":{"sys":{"cap":8192,"used":512,"rec":6,"app":6,"evic":0},
 *            "app":{"cap":23552,"used":880,"rec":11,"app":11,"evic":0}},
 *    "last":{"sys":{"rec":9,"used":712},"app":{"rec":240,"used":23544}},
 *    "dropped":0,"trunc":0,"refused":0,"unnamed":0,"hseq":41}
 *
 * Returns bytes written excluding the NUL, or 0 when `out` is too small for
 * even the shortest report (never a half-written document).
 */
size_t term_lp_report(char *out, size_t out_size);

/*
 * Content, as JSON, oldest first — the PRIVILEGED read. Both partitions in
 * SYS-then-APP order, each record rendered as one string:
 *
 *   "[<t_ms>] <sys|app>/<writer>: <text>"      ("~" prefixed when truncated)
 *
 *   {"src":"lastboot","ok":1,"from":0,
 *    "lines":["[142] sys/system: boot 7 reset=panic(4) prev=captured(6) ...",
 *             "[311] app/reading: page 42"],
 *    "records":2,"next":2,"more":0}
 *
 * The counts come AFTER `lines` because they are not known until the array
 * has been built to the buffer's edge; key order is not significant to a JSON
 * parser and the alternative is a two-pass render of the whole tail.
 *
 * `from` skips that many records of the dump (0 for the whole thing) and
 * `next` is the value to pass for the following chunk; `more` says whether
 * the buffer filled before the records ran out. Against LASTBOOT the cursor
 * is exact because the snapshot is frozen; against LIVE it is best-effort by
 * construction, which is why forensics pull LASTBOOT.
 *
 * `ok` is 0 with an empty `lines` when the source does not exist.
 *
 * `out_size` must be at least 96 bytes; TERM_LP_DUMP_MAX always fits at
 * least one record. A buffer too small for even one rendered line answers
 * `records: 0, more: 1`, so a chunking loop must advance on `records > 0`
 * rather than on `more` alone.
 */
size_t term_lp_dump_json(term_lp_src_t src, uint32_t from,
                         char *out, size_t out_size);

#ifdef __cplusplus
}
#endif
