/*
 * term_core — the VT parser + grid + scrollback, as pure logic.
 *
 * Phase 1 of docs/term-design.md §11: an I/O-independent C module that
 * builds and is tested on the host. Nothing in this header (or its
 * implementation) may include ESP-IDF, FreeRTOS or LVGL, and nothing here
 * draws, locks, owns a task, or knows about the term registry — those are
 * the upper layers' job (§3.1, §5, §9). This file is C99 with no external
 * dependencies beyond <stdint.h>/<stddef.h>/<stdbool.h>.
 *
 * The object is a single value type: bytes go in with term_core_feed(),
 * and the caller reads the grid, the dirty-row set and the scrollback back
 * out. Two callbacks go the other way: terminal replies (DSR/DA, §6) and
 * caret movement (§10.2, the ui.caret sink that ime_core reads).
 *
 * ---------------------------------------------------------------------
 * MEMORY CONTRACT (§4.2)
 * ---------------------------------------------------------------------
 * The caller supplies all memory. term_core_mem_size() reports how many
 * bytes a configuration needs; term_core_init() carves the core, both
 * grids, the scrollback arena and every index out of that one block. The
 * implementation calls malloc/realloc/free exactly zero times, at init and
 * ever after — that is the structural reason a leak or a fragmentation
 * path cannot exist here. The block must be 8-byte aligned and must stay
 * valid and unmoved for the lifetime of the core.
 *
 * The grid is sized once, for the worst-case cell count over every
 * orientation (§4.1: 80x53 portrait / 142x30 landscape => 4,260 cells), so
 * a rotation is a resize inside memory that already exists.
 *
 * ---------------------------------------------------------------------
 * PARSER HARD BOUNDS (§5) — these are contract, not implementation detail
 * ---------------------------------------------------------------------
 * The parser will run on the shared UI (LVGL) task, so a runaway parser is
 * a whole-device SPOF. Therefore:
 *
 *   B1. Processing one input byte is O(1). The state machine contains no
 *       loop over input. term_core_feed(core, p, n) is O(n) with a bound
 *       per byte that does not depend on the byte's value.
 *   B2. Numeric parameters that mean "do this many times" (REP, IL/DL,
 *       ICH/DCH/ECH, cursor motion, scroll counts) are clamped to the grid
 *       dimensions before use. "insert 2^31 lines" costs rows-worth of
 *       work, not 2^31-worth. B1 and B2 together are what make the byte
 *       budget in §5 a real budget.
 *   B3. CSI parameters: at most TERM_CSI_MAX_PARAMS (16) of them, each
 *       saturating at TERM_CSI_PARAM_MAX (65535). Excess parameters are
 *       dropped, not accumulated; a CSI whose parameter list overflows is
 *       still dispatched with the first 16 (same as real terminals).
 *   B4. OSC payload is buffered up to TERM_OSC_MAX_LEN bytes; beyond that
 *       the remainder is discarded, but the state machine still consumes
 *       the string to its terminator (BEL or ST) so the stream re-syncs.
 *       DCS/SOS/PM/APC payload is never buffered at all — it is discarded
 *       byte by byte until ST — so no length limit is needed there.
 *   B5. A resize never allocates, never fails for want of memory, and can
 *       only be refused for exceeding the maxima fixed at init.
 *   B6. No input sequence can make the core read or write outside the
 *       block given to term_core_init(). Phase 1 ships a fuzz test whose
 *       job is to try (§5, last bullet).
 *
 * ---------------------------------------------------------------------
 * WHAT THE CORE DOES NOT DO
 * ---------------------------------------------------------------------
 * No drawing, no glyphs, no PPA. No tasks, no queues, no mutexes: the core
 * is single-writer by construction and the caller guarantees it (§5, parse
 * happens on the drain). No registry, no generation/owner checks, no
 * quiesce, no MQTT, no LP SRAM black box, no JS bindings — all of that is
 * phase 2+ and lives above this header. No input/key handling at all: term
 * is the output surface only (§10.1).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===================================================================== */
/* Limits (§5 B3/B4, §4.1)                                               */
/* ===================================================================== */

#define TERM_CSI_MAX_PARAMS   16      /* B3: parameters kept per CSI      */
#define TERM_CSI_PARAM_MAX    65535u  /* B3: per-parameter saturation     */
#define TERM_OSC_MAX_LEN      256     /* B4: OSC payload bytes buffered   */
#define TERM_UTF8_MAX_PEND    4       /* bytes of a split sequence held   */

/* Worst-case geometry across orientations (§4.1). A configuration may ask
 * for less; asking for more is legal but costs proportionally more memory,
 * which is the caller's business since the caller owns the block. */
#define TERM_MAX_COLS_DEFAULT 142
#define TERM_MAX_ROWS_DEFAULT 53
#define TERM_MAX_CELLS_DEFAULT 4260   /* 80*53 == 142*30 == 4,260         */

/* ===================================================================== */
/* Colour (§4.1: cells hold RGB565 directly, never a palette index)      */
/* ===================================================================== */

/* Pack 8-bit RGB into RGB565, the canvas' native format. Rounding is
 * truncation, matching the existing ui.cells path. */
#define TERM_RGB565(r, g, b) \
    ((uint16_t)((((uint32_t)(r) & 0xF8u) << 8) | \
                (((uint32_t)(g) & 0xFCu) << 3) | \
                 (((uint32_t)(b) & 0xF8u) >> 3)))

/*
 * The 16-colour palette, unified here (§1: three identical copies exist
 * today in ui_tab5.cpp, ssh_vt.js and skk_test.js; §6: "the palette is
 * consolidated here"). Values are the 24-bit constants from
 * examples/ssh_vt.js PALETTE, converted to RGB565 — reading that file for
 * these numbers is the one sanctioned implementation reference for phase 1.
 *
 * Index order is ANSI: 0-7 normal, 8-15 bright. SGR "bold" maps a normal
 * foreground to its bright twin (index | 8) at ingest time, so the blitter
 * needs no bold handling (§6).
 */
static const uint16_t term_pal16[16] = {
    /* 0 black   0x55606B */ 0x530D,
    /* 1 red     0xE05A4E */ 0xE2C9,
    /* 2 green   0x2ECC71 */ 0x2E6E,
    /* 3 yellow  0xFFD479 */ 0xFEAF,
    /* 4 blue    0x4FC3F7 */ 0x4E1E,
    /* 5 magenta 0xC678DD */ 0xC3DB,
    /* 6 cyan    0x56B6C2 */ 0x55B8,
    /* 7 white   0xC9D1D9 */ 0xCE9B,
    /* 8 br.blk  0x8B98A5 */ 0x8CD4,
    /* 9 br.red  0xFF6B5E */ 0xFB4B,
    /*10 br.grn  0x4AE38A */ 0x4F11,
    /*11 br.yel  0xFFE08A */ 0xFF11,
    /*12 br.blu  0x6FD3FF */ 0x6E9F,
    /*13 br.mag  0xD898E8 */ 0xDCDD,
    /*14 br.cyn  0x7FD8E0 */ 0x7EDC,
    /*15 br.wht  0xFFFFFF */ 0xFFFF,
};

/* Bounds-clamped accessor. Also keeps term_pal16 "used" in every
 * translation unit, so a build at -Wunused-const-variable=2 stays clean. */
static inline uint16_t term_pal16_at(int i)
{
    return term_pal16[(unsigned)i & 15u];
}

#define TERM_COLOR_FG_DEFAULT  0xCE9B  /* 0xC9D1D9, ssh_vt.js FG          */
#define TERM_COLOR_BG_DEFAULT  0x0862  /* 0x0B0E11, ssh_vt.js BG          */
#define TERM_COLOR_CURSOR      0x4E1E  /* 0x4FC3F7, ssh_vt.js CURSOR      */

/* SGR 38;5;n / 48;5;n (§6). 0-15 -> term_pal16, 16-231 -> the 6x6x6 cube,
 * 232-255 -> the greyscale ramp; all three land in RGB565 directly, with no
 * quantisation back down to 16 colours (§4.1 rejects that as a loss below
 * panel capability). Out-of-range n returns TERM_COLOR_FG_DEFAULT. */
uint16_t term_color_xterm256(int n);

/* ===================================================================== */
/* Cell (§4.1, 12 bytes exactly)                                         */
/* ===================================================================== */

/* Codepoint sentinel for the trailing half of a double-width character.
 * Chosen just past the Unicode maximum so it can never collide with real
 * text. A CONT cell carries the lead cell's fg/bg so a reverse-video run
 * paints continuously, and carries TERM_CELL_CONT in flags. */
#define TERM_CP_CONT 0x00110000u

enum {
    TERM_CELL_WIDE      = 1u << 0,  /* lead half of a 2-column character  */
    TERM_CELL_CONT      = 1u << 1,  /* trailing half; cp == TERM_CP_CONT  */
    TERM_CELL_BOLD      = 1u << 2,  /* SGR 1 (already folded into fg)     */
    TERM_CELL_DIM       = 1u << 3,  /* SGR 2                              */
    TERM_CELL_ITALIC    = 1u << 4,  /* SGR 3                              */
    TERM_CELL_UNDERLINE = 1u << 5,  /* SGR 4 — the blitter draws the rule */
    TERM_CELL_REVERSE   = 1u << 6,  /* SGR 7 (already folded into fg/bg)  */
    TERM_CELL_STRIKE    = 1u << 7,  /* SGR 9                              */
};

/*
 * fg/bg are FINAL colours: reverse has already swapped them and bold has
 * already mapped the foreground to its bright twin, at the moment the cell
 * was written. The blitter therefore needs no attribute logic beyond the
 * underline rule (§6, "no blitter change"). The flags are kept anyway so a
 * snapshot/probe can reconstruct the attributes it was rendered with.
 */
typedef struct {
    uint32_t cp;     /* codepoint, or TERM_CP_CONT; 0x20 for a blank */
    uint16_t fg;     /* RGB565 */
    uint16_t bg;     /* RGB565 */
    uint16_t flags;  /* TERM_CELL_* */
    uint16_t _pad;
} term_cell_t;       /* 12B — asserted in the implementation */

/* Attribute run over a scrollback logical line: `cells` consecutive cells
 * share fg/bg/flags. Runs tile the line in order and sum to its length.
 * This is the storage form (§4.1 budgets ~48B per logical line, which only
 * works because scrollback holds UTF-8 text plus runs, not 12B cells). */
typedef struct {
    uint16_t cells;
    uint16_t fg;
    uint16_t bg;
    uint16_t flags;
} term_attr_run_t;   /* 8B */

/* ===================================================================== */
/* Modes (queryable state, §6)                                           */
/* ===================================================================== */

enum {
    TERM_MODE_CURSOR_VISIBLE = 1u << 0, /* DECTCEM, DECSET 25; default on */
    TERM_MODE_BRACKETED      = 1u << 1, /* DECSET 2004                    */
    TERM_MODE_ALT_SCREEN     = 1u << 2, /* DECSET 1049 currently active   */
    TERM_MODE_APP_CURSOR     = 1u << 3, /* DECCKM 1 — read by the input   */
                                        /* layer; the core never sends    */
                                        /* keys itself (§10.1)            */
    TERM_MODE_AUTOWRAP       = 1u << 4, /* DECAWM 7; default on           */
    TERM_MODE_ORIGIN         = 1u << 5, /* DECOM 6, CUP relative to the   */
                                        /* scroll region                  */
};
/* Mouse reporting (1000/1006) is deliberately absent: §6 defers it to a
 * later phase, and adding the flag now would invite half an implementation. */

/* ===================================================================== */
/* Configuration and construction                                        */
/* ===================================================================== */

/* Named TERM_VT/TERM_LOG as in §3.1's struct sketch, and deliberately NOT
 * sharing the TERM_MODE_* prefix used by the DECSET flags above — two
 * different concepts under one prefix is a mis-constant waiting to happen. */
typedef enum {
    /* Grid is the primary store; main + alt screens both allocated;
     * lines pushed off the top are joined by soft-wrap and archived. */
    TERM_VT = 0,
    /* Scrollback is the primary store; no alt screen is allocated, and
     * DECSET 1049 is accepted-and-ignored. The grid is the current-width
     * tail view. Only the allocation and the 1049 behaviour differ; the
     * parser is the same one (§3.2, "two modes, one core"). */
    TERM_LOG = 1,
} term_mode_t;

/* Terminal replies: DSR 5n/6n and primary DA (§6). Emitted from inside
 * term_core_feed(). Bytes are valid only for the duration of the call —
 * copy if you need to keep them. The upper layer routes them to the ssh
 * channel (pipe) or to term.onReply (JS feed). May be NULL, in which case
 * replies are generated and dropped. */
typedef void (*term_reply_fn)(void *user, const char *bytes, size_t len);

/* Caret change notification (§10.2). Fires only when (col,row,visible)
 * differs from the last notified value, at most once per feed()/resize()
 * call, after the grid is consistent. This is the C-to-C equivalent of a
 * JS app calling ui.caret(); ime_core reads that sink and never learns
 * that term exists. May be NULL. */
typedef void (*term_caret_fn)(void *user, int col, int row, bool visible);

typedef struct {
    term_mode_t mode;

    int cols;             /* initial geometry; 1..max_cols               */
    int rows;             /* 1..max_rows                                 */

    /* Allocation maxima. cols*rows must be <= max_cells at all times, so
     * a portrait/landscape pair costs the max of the two products, not
     * the product of the maxima (§4.1). 0 for any of the three selects
     * TERM_MAX_*_DEFAULT. */
    int max_cols;
    int max_rows;
    int max_cells;

    /* Scrollback (§4.1: 48KB arena, 1,024 line index by default). Both 0
     * disables scrollback entirely: lines pushed off the top are dropped
     * and term_core_sb_first() == term_core_sb_end(). */
    size_t scrollback_bytes;
    int scrollback_lines;

    term_reply_fn reply_cb;
    void         *reply_user;
    term_caret_fn caret_cb;
    void         *caret_user;
} term_config_t;

/* Opaque; the caller never sees the layout, only the byte count. */
typedef struct term_core term_core_t;

/* Bytes term_core_init() needs for this configuration, or 0 if the
 * configuration is invalid (geometry out of range, cols*rows > max_cells,
 * a scrollback arena too small for one maximum-width line, ...). Pure
 * function of cfg; safe to call before any allocation exists. */
size_t term_core_mem_size(const term_config_t *cfg);

/* Place a core in `mem` (>= term_core_mem_size(cfg) bytes, 8-byte
 * aligned). Returns a handle into that block, or NULL if the block is too
 * small/misaligned or cfg is invalid. The block is fully initialised: the
 * grid is blank in the default colours, the cursor is at (0,0) and
 * visible, autowrap is on, the scroll region is the whole screen. No
 * allocation happens here or afterwards. */
term_core_t *term_core_init(void *mem, size_t mem_size, const term_config_t *cfg);

/* RIS: blank both screens, reset SGR, modes, scroll region and cursor.
 * Scrollback is NOT cleared (a reset is not a history wipe); use
 * term_core_scrollback_clear() for that. Marks a full repaint. */
void term_core_reset(term_core_t *c);

/* Late binding of the callbacks — the registry re-points these when a
 * persist term is re-attached to a new owner-side consumer. Either may be
 * NULL to detach. */
void term_core_set_reply_cb(term_core_t *c, term_reply_fn fn, void *user);
void term_core_set_caret_cb(term_core_t *c, term_caret_fn fn, void *user);

/* There is no term_core_free(): the core owns nothing. The caller
 * releases the block whenever it has established that no one is feeding
 * or reading (the quiesce protocol of §3.1, which is phase 2). */

/* ===================================================================== */
/* Input                                                                 */
/* ===================================================================== */

/*
 * Feed raw bytes. This is the only mutating entry point besides resize()
 * and reset().
 *
 * UTF-8 split resilience (§6, "new"): `bytes` may cut a multi-byte
 * sequence at any boundary. Up to TERM_UTF8_MAX_PEND bytes of an
 * incomplete sequence are carried across calls, and so is every escape
 * state — feeding one byte at a time must produce exactly the same grid as
 * feeding the whole buffer at once. Phase 1 tests assert that equivalence
 * over every split point.
 *
 * Malformed input never desynchronises: an invalid, overlong, surrogate or
 * out-of-range sequence emits one U+FFFD per maximal invalid subpart and
 * resumes at the byte that ended it (that byte is re-examined, not eaten).
 *
 * Cost is O(len) with the per-byte bound of B1/B2. Callers implement the
 * per-frame byte budget (§5) by slicing their buffer; the core imposes no
 * budget of its own and always consumes the whole slice.
 */
void term_core_feed(term_core_t *c, const uint8_t *bytes, size_t len);

/* ===================================================================== */
/* Geometry                                                              */
/* ===================================================================== */

int term_core_cols(const term_core_t *c);
int term_core_rows(const term_core_t *c);

/*
 * Resize to cols x rows (§4.1 rotation path). Returns 0, or -1 if the
 * geometry exceeds the maxima fixed at init (B5) — never for want of
 * memory, since nothing is allocated.
 *
 * Content is carried over, not discarded: the device lesson recorded in
 * ssh_vt.js is that dropping the grid and waiting for the remote to
 * redraw leaves a plain shell prompt on a blank screen. Cells outside the
 * new box are pushed into scrollback (rows lost off the top) or truncated
 * (columns lost off the right, following the soft-wrap rule below); new
 * area is blanked in the current default colours.
 *
 * Scrollback is stored as logical lines with their soft-wrap structure
 * already joined, so it needs no rewriting here — it is re-wrapped on read
 * at the current width (term_core_line_seg_count/term_core_line_segment).
 * That is what stops a rotation from freezing yesterday's history at
 * yesterday's column count (§3.1).
 *
 * Marks a full repaint. Clamps the cursor and the scroll region into the
 * new box. Alt-screen content is resized the same way but is never
 * archived to scrollback.
 */
int term_core_resize(term_core_t *c, int cols, int rows);

/* ===================================================================== */
/* Grid readout                                                          */
/* ===================================================================== */

/* Row `row` (0 = top) of the ACTIVE screen: `cols` contiguous cells, valid
 * until the next mutating call. NULL if row is out of range. The blitter's
 * fast path — one row is 142*12 = 1,704 contiguous bytes (§4.3). */
const term_cell_t *term_core_row(const term_core_t *c, int row);

/* Row flags. SOFTWRAP means "this row continues into the next one"; it is
 * what lets the archiver rebuild a logical line. */
enum {
    TERM_ROW_SOFTWRAP = 1u << 0,
};
uint32_t term_core_row_flags(const term_core_t *c, int row);

/* Cursor position and visibility. Any of the out pointers may be NULL. */
void term_core_cursor(const term_core_t *c, int *col, int *row, bool *visible);

/* Bitwise OR of TERM_MODE_* flags currently set. */
uint32_t term_core_modes(const term_core_t *c);

/* True while DECSET 1049 is active. Writes then go to the alt screen and
 * NOTHING reaches scrollback until 1049 is reset — that non-pollution is
 * the contract, not a side effect (§3.2, §6). */
bool term_core_alt_active(const term_core_t *c);

/* Current DECSTBM region, inclusive, 0-based. */
void term_core_scroll_region(const term_core_t *c, int *top, int *bot);

/* Render row `row` of the active screen as UTF-8 into `out` (§7.2 format:
 * one line per row, CONT cells skipped, trailing blanks trimmed, no
 * attributes). NUL-terminates when out_size > 0. Returns the number of
 * bytes the row needs excluding the NUL — a value >= out_size means the
 * output was truncated. Never mutates the core: a probe must not perturb
 * what it measures. */
int term_core_row_utf8(const term_core_t *c, int row, char *out, size_t out_size);

/* ===================================================================== */
/* Dirty rows (§5, §9: blit only what changed)                           */
/* ===================================================================== */

bool term_core_row_dirty(const term_core_t *c, int row);

/* First dirty row >= from_row, or -1 when there are none. O(rows/32), so
 * the usual `for (r = term_core_dirty_next(c, 0); r >= 0;
 *                r = term_core_dirty_next(c, r + 1))` walk is cheap. */
int term_core_dirty_next(const term_core_t *c, int from_row);

/* Set after resize, reset and any alt-screen switch: the damage set cannot
 * describe those, so the caller must repaint everything. */
bool term_core_full_repaint(const term_core_t *c);

/* Clear the dirty set and the full-repaint flag. The renderer calls this
 * after it has blitted, and it is the ONLY reader-side call that mutates
 * the core — keep it out of probe paths. */
void term_core_dirty_clear(term_core_t *c);

/* ===================================================================== */
/* Scrollback: logical lines (§3.1, §3.2)                                */
/* ===================================================================== */

/*
 * A scrollback entry is a LOGICAL line: when rows leave the top of the
 * grid, a run joined by TERM_ROW_SOFTWRAP is concatenated into one entry
 * before being archived. Entries are addressed by a monotonically
 * increasing uint32 id that is never reused, so a reader that cached ids
 * can tell eviction (id < sb_first) from a bad id. Ids survive resize.
 */

/* Id of the oldest surviving entry, and one past the newest. Empty when
 * they are equal. */
uint32_t term_core_sb_first(const term_core_t *c);
uint32_t term_core_sb_end(const term_core_t *c);

/* Length of entry `id` in CELLS (double-width characters count 2), or -1
 * if the id has been evicted or was never written. */
int term_core_line_length(const term_core_t *c, uint32_t id);

/* Entry `id` as UTF-8, same formatting rules as term_core_row_utf8().
 * Returns the byte length excluding the NUL, or -1 for a dead id. This is
 * the serialiser behind term.read()'s chunk fetch (§7.2, §8). */
int term_core_line_utf8(const term_core_t *c, uint32_t id, char *out, size_t out_size);

/* Attribute runs of entry `id`, in order, up to max_runs of them. Returns
 * the total number of runs the line has (> max_runs means truncated), or
 * -1 for a dead id. `out` may be NULL when max_runs is 0, to count first. */
int term_core_line_attrs(const term_core_t *c, uint32_t id,
                         term_attr_run_t *out, int max_runs);

/*
 * Re-wrapping on read. A logical line occupies term_core_line_seg_count()
 * display rows at the CURRENT width; term_core_line_segment() materialises
 * one of them as cells, applying the same wide-character rule as live
 * input (a 2-column character never straddles the right edge; the cell
 * before the break is left blank). This pair is how the history view stays
 * correct across a rotation (§3.1) and how TERM_LOG renders at all (§3.2).
 *
 * seg_count returns >= 1 for a live id (an empty line is one blank row),
 * -1 for a dead id. segment() returns the number of cells written into
 * `out` (<= min(cols, max_cells)), or -1 for a dead id or an out-of-range
 * seg. Neither mutates the core.
 */
int term_core_line_seg_count(const term_core_t *c, uint32_t id);
int term_core_line_segment(const term_core_t *c, uint32_t id, int seg,
                           term_cell_t *out, int max_cells);

/* Drop all scrollback. Ids continue from where they were (never reused). */
void term_core_scrollback_clear(term_core_t *c);

/* ===================================================================== */
/* Counters (fuzz/probe visibility; never affect behaviour)              */
/* ===================================================================== */

typedef struct {
    uint64_t bytes_in;          /* bytes handed to feed()                */
    uint64_t chars_written;     /* codepoints placed into the grid       */
    uint32_t lines_archived;    /* logical lines pushed to scrollback    */
    uint32_t lines_evicted;     /* archived lines dropped by the ring    */
    uint32_t utf8_errors;       /* U+FFFD substitutions made             */
    uint32_t csi_overflow;      /* CSIs that hit the 16-parameter cap    */
    uint32_t osc_truncated;     /* OSC payloads that hit TERM_OSC_MAX_LEN*/
    uint32_t seq_ignored;       /* escape sequences parsed and dropped   */
    uint32_t replies;           /* DSR/DA replies emitted                */
} term_stats_t;

void term_core_stats(const term_core_t *c, term_stats_t *out);

#ifdef __cplusplus
}
#endif
