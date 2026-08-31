/*
 * edit_lex.c — the per-line JavaScript lexer and the state cascade.
 *
 * A line is lexed from the state its head is in (line_state[]).  Only two
 * constructs survive a newline — block comments and template literals — so
 * the state is two bits, and the top bit of the same byte says "this line's
 * head state may be stale" (spec §A.1).
 *
 * Nothing is lexed on the editing path.  An edit marks the line below the
 * edited one stale and returns; the cascade is run by edit_view_row through
 * lex_ensure(), which is the only place that knows where the visible region
 * ends.  That is what bounds the work of opening a block comment at the top
 * of a 4000 line file: the cascade stops at the bottom of the screen and
 * leaves one stale marker behind, and scrolling down resolves the rest a
 * screen at a time.
 *
 * Regular-expression literals are deliberately not recognised: deciding
 * whether '/' opens a regex needs the previous token's grammatical role,
 * which does not close inside a line, and getting it wrong would poison the
 * line state itself.  '/' is PUNCT (spec §A.1, §E-12).
 */

#include "edit_internal.h"

static bool is_space(uint8_t c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f';
}

static bool is_digit(uint8_t c)
{
    return c >= '0' && c <= '9';
}

static bool is_ident_start(uint8_t c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           c == '_' || c == '$' || c >= 0x80u;
}

static bool is_ident_part(uint8_t c)
{
    return is_ident_start(c) || is_digit(c);
}

/*
 * Reserved words plus the three literals and `undefined`, which readers of a
 * script expect to see coloured like keywords.  Kept as one flat table: an
 * identifier costs one strcmp per same-length entry, and a row of 142 cells
 * holds a couple of dozen identifiers at most.
 */
static const char *const KEYWORDS[] = {
    "as", "async", "await", "break", "case", "catch", "class", "const",
    "continue", "debugger", "default", "delete", "do", "else", "export",
    "extends", "false", "finally", "for", "from", "function", "get", "if",
    "import", "in", "instanceof", "let", "new", "null", "of", "return",
    "set", "static", "super", "switch", "this", "throw", "true", "try",
    "typeof", "undefined", "var", "void", "while", "with", "yield",
};

static bool is_keyword(const struct edit *e, uint32_t from, uint32_t to)
{
    char t[16];
    uint32_t n = to - from;

    if (n < 2u || n > 10u)
        return false;
    eb_copy_out(e, from, n, t);
    t[n] = '\0';
    for (size_t i = 0; i < sizeof KEYWORDS / sizeof KEYWORDS[0]; i++) {
        if (strlen(KEYWORDS[i]) != (size_t)n)
            continue;
        if (memcmp(KEYWORDS[i], t, (size_t)n) == 0)
            return true;
    }
    return false;
}

void lex_begin(lex_iter_t *it, const struct edit *e, uint32_t from, uint32_t end,
               uint8_t state)
{
    it->e     = e;
    it->pos   = from;
    it->end   = end;
    it->state = (uint8_t)(state & EDIT_LEX_MASK);
}

/* Scan for the two-byte closer of a block comment. */
static uint32_t find_block_end(const struct edit *e, uint32_t p, uint32_t end)
{
    while (p + 1u < end) {
        if (eb_at(e, p) == '*' && eb_at(e, p + 1u) == '/')
            return p + 2u;
        p++;
    }
    return end + 1u;   /* > end: not on this line */
}

/* Scan for an unescaped `q`; returns the offset just past it, or end+1. */
static uint32_t find_quote_end(const struct edit *e, uint32_t p, uint32_t end, uint8_t q)
{
    while (p < end) {
        uint8_t c = eb_at(e, p);
        if (c == '\\') {
            p += 2u;
            continue;
        }
        if (c == q)
            return p + 1u;
        p++;
    }
    return end + 1u;
}

bool lex_next(lex_iter_t *it, uint32_t *tok_from, uint32_t *tok_to, uint8_t *cls)
{
    const struct edit *e = it->e;
    uint32_t p = it->pos, end = it->end, q;
    uint8_t c;

    if (p >= end)
        return false;

    *tok_from = p;

    if (it->state == EDIT_LEX_BLOCK) {
        q = find_block_end(e, p, end);
        if (q <= end) {
            it->state = EDIT_LEX_PLAIN;
            it->pos = q;
        } else {
            it->pos = end;
        }
        *tok_to = it->pos;
        *cls = (uint8_t)EDIT_CLS_COMMENT;
        return true;
    }
    if (it->state == EDIT_LEX_TEMPLATE) {
        q = find_quote_end(e, p, end, (uint8_t)'`');
        it->pos = (q <= end) ? q : end;
        if (q <= end)
            it->state = EDIT_LEX_PLAIN;
        *tok_to = it->pos;
        *cls = (uint8_t)EDIT_CLS_STRING;
        return true;
    }

    c = eb_at(e, p);

    if (is_space(c)) {
        while (p < end && is_space(eb_at(e, p)))
            p++;
        it->pos = p;
        *tok_to = p;
        *cls = (uint8_t)EDIT_CLS_PLAIN;
        return true;
    }

    if (c == '/' && p + 1u < end && eb_at(e, p + 1u) == '/') {
        it->pos = end;
        *tok_to = end;
        *cls = (uint8_t)EDIT_CLS_COMMENT;
        return true;
    }
    if (c == '/' && p + 1u < end && eb_at(e, p + 1u) == '*') {
        q = find_block_end(e, p + 2u, end);
        if (q <= end) {
            it->pos = q;
        } else {
            it->pos = end;
            it->state = EDIT_LEX_BLOCK;
        }
        *tok_to = it->pos;
        *cls = (uint8_t)EDIT_CLS_COMMENT;
        return true;
    }
    if (c == '"' || c == '\'') {
        /* A plain string never survives a newline in JS, so an unterminated
         * one simply ends at the line end and the state stays PLAIN. */
        q = find_quote_end(e, p + 1u, end, c);
        it->pos = (q <= end) ? q : end;
        *tok_to = it->pos;
        *cls = (uint8_t)EDIT_CLS_STRING;
        return true;
    }
    if (c == '`') {
        q = find_quote_end(e, p + 1u, end, c);
        if (q <= end) {
            it->pos = q;
        } else {
            it->pos = end;
            it->state = EDIT_LEX_TEMPLATE;
        }
        *tok_to = it->pos;
        *cls = (uint8_t)EDIT_CLS_STRING;
        return true;
    }
    if (is_digit(c) || (c == '.' && p + 1u < end && is_digit(eb_at(e, p + 1u)))) {
        p++;
        while (p < end) {
            uint8_t d = eb_at(e, p);
            if (is_ident_part(d) || d == '.') {
                bool exp = (d == 'e' || d == 'E' || d == 'p' || d == 'P');
                p++;
                if (exp && p < end && (eb_at(e, p) == '+' || eb_at(e, p) == '-'))
                    p++;
                continue;
            }
            break;
        }
        it->pos = p;
        *tok_to = p;
        *cls = (uint8_t)EDIT_CLS_NUMBER;
        return true;
    }
    if (is_ident_start(c)) {
        while (p < end && is_ident_part(eb_at(e, p)))
            p++;
        it->pos = p;
        *tok_to = p;
        *cls = is_keyword(e, *tok_from, p) ? (uint8_t)EDIT_CLS_KEYWORD
                                           : (uint8_t)EDIT_CLS_IDENT;
        return true;
    }

    it->pos = p + 1u;
    *tok_to = it->pos;
    *cls = (uint8_t)EDIT_CLS_PUNCT;
    return true;
}

uint8_t lex_line_end_state(const struct edit *e, uint32_t line, uint8_t start_state)
{
    lex_iter_t it;
    uint32_t a, b;
    uint8_t k;

    lex_begin(&it, e, eb_line_start(e, line), eb_line_end(e, line), start_state);
    while (lex_next(&it, &a, &b, &k))
        ;
    return it.state;
}

void lex_touch(struct edit *e, uint32_t line)
{
    uint32_t next = line + 1u;

    if (next < e->line_count)
        e->line_state[next] |= (uint8_t)EDIT_LEX_DIRTY;
    /* Lines can also have moved under an existing marker; never let the lower
     * bound sit above the edit. */
    if (e->lex_dirty_min > next)
        e->lex_dirty_min = next;
    if (e->lex_dirty_min > e->line_count)
        e->lex_dirty_min = e->line_count;

    /* §A.1: the edited line is re-lexed now and the cascade runs while the
     * end state keeps changing — but only to the bottom of the visible area.
     * Whatever is left below stays marked and is resolved by the first
     * edit_view_row() that needs it. */
    lex_ensure(e, e->top_line + (uint32_t)e->view_rows - 1u);
}

void lex_ensure(struct edit *e, uint32_t upto)
{
    uint32_t j;

    if (e->line_count == 0)
        return;
    if (upto >= e->line_count)
        upto = e->line_count - 1u;
    if (e->lex_dirty_min > upto)
        return;

    for (j = e->lex_dirty_min; j <= upto; j++) {
        uint8_t s;

        if (!(e->line_state[j] & EDIT_LEX_DIRTY))
            continue;
        s = (j == 0) ? (uint8_t)EDIT_LEX_PLAIN
                     : lex_line_end_state(e, j - 1u,
                                          (uint8_t)(e->line_state[j - 1u] & EDIT_LEX_MASK));
        for (;;) {
            e->line_state[j] = s;              /* clears the stale bit */
            e->stats.relex_lines++;
            if (j + 1u >= e->line_count) {
                j = upto;
                break;
            }
            s = lex_line_end_state(e, j, s);
            if (e->line_state[j + 1u] == s)    /* converged: below is still valid */
                break;
            if (j + 1u > upto) {               /* defer past the visible region */
                e->line_state[j + 1u] |= (uint8_t)EDIT_LEX_DIRTY;
                e->stats.relex_cascades++;
                break;
            }
            j++;
        }
    }
    e->lex_dirty_min = (upto + 1u < e->line_count) ? upto + 1u : e->line_count;
}
