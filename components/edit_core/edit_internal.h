/*
 * edit_internal.h — the shared shape of the editor core.  Not installed:
 * nothing outside components/edit_core/ may include this.
 *
 * ---------------------------------------------------------------- layout
 * One caller block holds, in this order and each 8-byte aligned:
 *
 *   struct edit                       the header below
 *   uint8_t  base[8 + cap + 8]        canary, text buffer, canary
 *   uint32_t line_start[max_lines]    physical offsets, gap included
 *   uint8_t  line_state[max_lines]    line-head lexer state + recompute bit
 *   uint8_t  undo[undo_bytes]         the undo/redo ring
 *
 * `cap` is cfg.max_bytes + 1.  The extra byte is what keeps the gap from ever
 * reaching size 0, so "text length <= max_bytes" and "gap >= 1" are the same
 * statement (spec §A.1).
 *
 * ------------------------------------------------- physical vs logical
 * A *logical* offset counts text bytes and knows nothing about the gap; it is
 * what every public function speaks.  A *physical* offset indexes buf[] and
 * therefore straddles the gap.  line_start[] holds physical offsets on
 * purpose: inserting inside the gap moves neither half of the buffer, so the
 * lines after the cursor keep their entries and a keystroke stays O(1) in the
 * number of lines (spec §A.1, "gap 込みの物理オフセット").
 *
 * The mapping is
 *     phys(L) = L < gap_begin ? L : L + gap_size
 *     logi(p) = p < gap_begin ? p : p - gap_size          (p not inside the gap)
 * so a line that starts exactly at the gap is stored as gap_end, never as
 * gap_begin: invariant 2 ("no line_start points into the gap") is then a
 * strict `p < gap_begin || p >= gap_end`.
 *
 * line_start[0] is the one exception.  It is always literally 0 — invariant 2
 * demands it, and with the gap at the very front phys(0) would otherwise be
 * gap_end.  Index 0 is never converted, never shifted and never removed.
 */
#ifndef EDIT_INTERNAL_H
#define EDIT_INTERNAL_H

#include "edit_core.h"

#include <string.h>

/*
 * The single source of truth for "how many columns does this codepoint
 * occupy".  Header-only and free of ESP-IDF headers, which is why the
 * dependency-free rule for edit_core survives including it; the alternative
 * is a fourth copy of the table, and docs/skk-ime-design.md §9.4 records what
 * happens when two copies drift (text shears one column at a time, silently).
 * components/term_core/term_core.c includes it by the same relative path.
 */
#include "../ui_tab5/include/ui_cell_width.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EDIT_PREEDIT_MAX   128u   /* spec §A.1: "core が持つ 128 B の小バッファ" */
#define EDIT_CANARY_LEN    8u     /* invariant 6 */
#define EDIT_TAB_WIDTH     4u     /* display only; see edit_view.c */
#define EDIT_MAX_ROWS_HARD 64u    /* edit_dirty_rows() is a uint64 bitmask */

/* line_state[] byte: low bits = lexer state at the line head, top bit = the
 * "needs recompute" flag of spec §A.1. */
#define EDIT_LEX_MASK      0x03u
#define EDIT_LEX_DIRTY     0x80u
#define EDIT_LEX_PLAIN     0u
#define EDIT_LEX_BLOCK     1u     /* inside a block comment */
#define EDIT_LEX_TEMPLATE  2u     /* inside a template literal */

/* undo record kinds */
#define EDIT_U_INSERT      1u
#define EDIT_U_DELETE      2u
/* record flag: this record is the tail half of a group (a selection replace);
 * undo/redo keep going into the record before it. */
#define EDIT_U_CHAIN       0x01u

#define EDIT_U_HDR         12u    /* kind, flags, reserved, off, len */
#define EDIT_U_FOOT        4u     /* total size, so the ring reads backwards */
#define EDIT_U_OVERHEAD    (EDIT_U_HDR + EDIT_U_FOOT)

#define EDIT_NO_GOAL       0xFFFFFFFFu

struct edit {
    edit_config_t cfg;

    uint8_t  *base;        /* canary, buf, canary */
    uint8_t  *buf;         /* base + EDIT_CANARY_LEN, cap bytes */
    uint32_t  cap;         /* cfg.max_bytes + 1 */
    uint32_t  gap_begin;
    uint32_t  gap_end;

    uint32_t *line_start;  /* physical, ascending; [0] is always 0 */
    uint8_t  *line_state;
    uint32_t  line_count;  /* >= 1 */

    uint32_t  cur;         /* cursor, logical byte offset, on a char boundary */
    uint32_t  cur_line;    /* line containing cur */
    uint32_t  cur_col;     /* cached cell column of the cursor (see ev_cursor_col) */
    bool      cur_col_ok;  /* false = cur_col has to be walked again */
    uint32_t  goal_col;    /* remembered cell column for UP/DOWN, or EDIT_NO_GOAL */

    uint32_t  sel_anchor;  /* logical byte offset */
    bool      sel_active;

    uint32_t  mark_line1;  /* 0 = no error mark */
    uint32_t  mark_col1;

    uint8_t   preedit[EDIT_PREEDIT_MAX];
    uint16_t  preedit_len;

    uint16_t  view_cols, view_rows;
    uint32_t  top_line;    /* first line on screen */
    uint32_t  left_col;    /* horizontal scroll, in cells */
    uint32_t  dirty_flags;
    uint64_t  dirty_rows;

    uint32_t  lex_dirty_min;  /* lower bound on the first line with EDIT_LEX_DIRTY */

    uint8_t  *undo;
    uint32_t  undo_cap;
    uint64_t  u_head;      /* oldest recorded byte */
    uint64_t  u_tail;      /* end of the newest undoable record */
    uint64_t  u_top;       /* end of the newest record; [tail,top) is redo */
    bool      u_applying;  /* inside undo/redo: do not record */
    bool      u_coalesce;  /* the newest record may still absorb a typed character */

    bool      modified;
    uint32_t  edit_count;
    edit_stats_t stats;
};

/* ------------------------------------------------------------ edit_buf.c */

static inline uint32_t eb_gap(const struct edit *e) { return e->gap_end - e->gap_begin; }
static inline uint32_t eb_len(const struct edit *e) { return e->cap - eb_gap(e); }

static inline uint32_t eb_phys(const struct edit *e, uint32_t l)
{
    return (l < e->gap_begin) ? l : l + eb_gap(e);
}
static inline uint32_t eb_logi(const struct edit *e, uint32_t p)
{
    return (p < e->gap_begin) ? p : p - eb_gap(e);
}
static inline uint8_t eb_at(const struct edit *e, uint32_t l)
{
    return e->buf[eb_phys(e, l)];
}
static inline uint32_t eb_line_start(const struct edit *e, uint32_t i)
{
    return (i == 0) ? 0u : eb_logi(e, e->line_start[i]);
}
/* End of line i, exclusive of its newline. */
static inline uint32_t eb_line_end(const struct edit *e, uint32_t i)
{
    return (i + 1 < e->line_count) ? eb_line_start(e, i + 1) - 1u : eb_len(e);
}

void     eb_stamp_canary(struct edit *e);
bool     eb_canary_ok(const struct edit *e);   /* invariant 6 */
void     eb_move_gap(struct edit *e, uint32_t l);
uint32_t eb_find_line(const struct edit *e, uint32_t l);
void     eb_copy_out(const struct edit *e, uint32_t from, uint32_t n, void *dst);
void     eb_insert_raw(struct edit *e, const char *s, uint32_t n);
void     eb_delete_raw(struct edit *e, uint32_t a, uint32_t b);

/* UTF-8.  The text is validated on the way in, so the decoders below may
 * assume well-formed input; they still refuse to run past the end. */
bool     eb_utf8_valid(const char *s, size_t n);
uint32_t eb_decode(const struct edit *e, uint32_t l, uint32_t *cp);   /* 0 at end of text */
uint32_t eb_next_char(const struct edit *e, uint32_t l);
uint32_t eb_prev_char(const struct edit *e, uint32_t l);
uint32_t eb_decode_mem(const char *s, uint32_t n, uint32_t *cp);
uint32_t eb_count_nl(const char *s, uint32_t n);
uint32_t eb_count_nl_range(const struct edit *e, uint32_t a, uint32_t b);

/* ----------------------------------------------------------- edit_undo.c */

void eu_reset(struct edit *e);
void eu_record_insert(struct edit *e, uint32_t off, const char *s, uint32_t n, bool chain);
void eu_record_delete(struct edit *e, uint32_t off, uint32_t n, bool chain);
bool eu_undo_step(struct edit *e);    /* false = nothing left to undo */
bool eu_redo_step(struct edit *e);
bool eu_walk(const struct edit *e);   /* invariant 5 */

/* ------------------------------------------------------------ edit_lex.c */

typedef struct {
    const struct edit *e;
    uint32_t pos, end;
    uint8_t  state;
} lex_iter_t;

void    lex_begin(lex_iter_t *it, const struct edit *e, uint32_t from, uint32_t end, uint8_t state);
bool    lex_next(lex_iter_t *it, uint32_t *tok_from, uint32_t *tok_to, uint8_t *cls);
uint8_t lex_line_end_state(const struct edit *e, uint32_t line, uint8_t start_state);
void    lex_touch(struct edit *e, uint32_t line);
void    lex_ensure(struct edit *e, uint32_t upto_line);

/* ----------------------------------------------------------- edit_view.c */

void     ev_mark_all(struct edit *e);
void     ev_mark_line(struct edit *e, uint32_t line);
void     ev_mark_from(struct edit *e, uint32_t line);   /* line and everything below */
void     ev_follow_cursor(struct edit *e);
uint32_t ev_cells_of(uint32_t cp, uint32_t col);
uint32_t ev_col_cells(const struct edit *e, uint32_t line, uint32_t upto);  /* cells before upto */
uint32_t ev_cursor_col(const struct edit *e);   /* cached; walks the line only when stale */
uint32_t ev_col_to_byte(const struct edit *e, uint32_t line, uint32_t cells);

#ifdef __cplusplus
}
#endif

#endif /* EDIT_INTERNAL_H */
