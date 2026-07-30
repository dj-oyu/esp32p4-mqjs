/*
 * ime_core — the IME session policy every input surface shares.
 *
 * skk_core converts. ui_overlay draws. Everything BETWEEN those two was
 * written three times: ssh_vt.js, skk_test.js and reading.js each carry
 * their own copy of "when is the IME armed", "what does the 「あ」 button
 * mean", "which accessor do I re-read for which status bit", "when does
 * the candidate bar fold away" — and, most importantly, "in what ORDER
 * are keys offered to the engine". That last one is not a style
 * question: a whole test harness (tools/ssh_vt_imetest.sh, deleted when
 * ssh_vt migrated) existed solely to watch it, in one of the three apps.
 *
 * This component is to skk_core what kbd_core is to the key surfaces.
 *
 * THE ORDERING RULE IS STRUCTURAL HERE, not a convention. A caller feeds
 * every key to ime_feed() FIRST and acts on what comes back, so an app
 * cannot expand "\0left" into "\x1b[D" before the engine has had its
 * say. That was the bug the ssh_vt script was written to catch; at this
 * layer it is unrepresentable.
 *
 * Pure logic: no I/O, no LVGL, no allocation, no clock. Same reason as
 * skk_core — these files are compiled by plain host gcc for the unit
 * test (tools/tests/test_ime_core.c), which is where the ordering
 * guarantee is actually verified.
 *
 * An ime_t is owned by ONE task (it embeds a skk_t, which is), so
 * nothing here locks.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "skk_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* What the caller must do with the key it just fed. */
typedef enum {
    /* Not consumed. The caller's own key path handles it exactly as if
       there were no IME — expand tokens, send to the wire, whatever.
       This is the common case and costs one call and no allocation. */
    IME_PASS = 0,
    /* Consumed. Nothing to deliver; the view may have changed (see
       ime_view()) so the renderer should look. */
    IME_TAKEN,
    /* Consumed AND produced committed text: ime_text(). The caller puts
       it wherever typed characters go — the SSH wire, a textarea, an
       edit buffer. Only this string ever leaves the IME; a preedit must
       never reach the wire. */
    IME_TEXT,
} ime_disp_t;

/* What changed since the last ime_view_clear(). The renderer reads
   these instead of re-deriving state, which is what keeps the candidate
   list from being rebuilt on every SPACE inside v mode (design §4.4). */
#define IME_V_PREEDIT (1u << 0) /* -> ime_preedit()          */
#define IME_V_CANDS   (1u << 1) /* -> ime_cand_count/ime_cand */
#define IME_V_SEL     (1u << 2) /* -> ime_sel()              */
#define IME_V_MODE    (1u << 3) /* -> ime_mode()             */
#define IME_V_ENABLE  (1u << 4) /* on/off flipped: repaint + tell the user */

typedef struct {
    skk_t    skk;
    bool     ready;  /* a dictionary is attached */
    bool     on;     /* kana input armed (the 「あ」 state) */
    int      mode;   /* cached skk_mode(), refreshed on SKK_ST_MODE only */
    int      ncand;  /* cached candidate count, for the fold-away rule */
    uint32_t view;   /* IME_V_* accumulated since ime_view_clear() */
    const char *text;    /* IME_TEXT: the committed string */
    size_t      text_len;
} ime_t;

/* Zero the session. Disabled, no dictionary; safe to feed immediately
   (everything is IME_PASS until a dictionary arrives and it is armed). */
void ime_init(ime_t *im);

/* Attach a dictionary opened by the caller (skk_dict_open), or NULL to
 * detach. `d` must outlive `im`.
 *
 * DELIBERATELY NOT AN OPEN: this component does no I/O, and the caller
 * is also the only one who knows whether a failed open may be
 * remembered. It may not be latched forever — skk_dict.bin is a
 * gitignored build input, so a clean-build firmware really does start
 * with no dictionary, and a permanent latch would keep the IME dead
 * until an app restart even after one is flashed. */
void ime_attach(ime_t *im, const skk_dict_t *d);
void ime_attach_mru(ime_t *im, skk_mru_t *m);

bool ime_ready(const ime_t *im);   /* has a dictionary */
bool ime_on(const ime_t *im);      /* kana input armed */

/* The 「あ」 toggle, i.e. what the control bar's "\0ime" token means.
 *
 * Turning it OFF discards a pending reading rather than committing it.
 * That is a decision, not an oversight (2026-07-29): a mis-tap loses a
 * few characters you can retype, whereas a mis-tap that COMMITS pushes
 * a string into a remote shell. Callers should say so — IME_V_ENABLE is
 * raised so the surface can show a toast.
 *
 * Learning is NOT written back here: ime_core does no I/O. The caller
 * persists on the same edge (that is where the user stops typing). */
void ime_set_on(ime_t *im, bool on);

/* Feed one key, exactly as the surface produced it — the same bytes
 * ui.onKey would see, and `len` is load-bearing because a "\0name"
 * token is recognised by key[0] == '\0'.
 *
 * Feed EVERYTHING here first, including tokens. The engine takes only
 * what it understands (arrows and ESC while a reading is open, so the
 * cursor cannot walk away from a preedit drawn at it) and passes the
 * rest straight back.
 *
 * "\0ime" is the one key this layer owns outright: it toggles and
 * returns IME_TAKEN whether or not the engine is armed. */
ime_disp_t ime_feed(ime_t *im, const char *key, size_t len);

/* Drop any pending reading (focus lost, the session being typed into
   went away, the screen was torn down). Discards, never commits. */
void ime_reset(ime_t *im);

/* ---- what to draw ---------------------------------------------------
 * All of these point into `im` or into the dictionary image and copy
 * nothing. They are valid only until the next ime_feed()/ime_reset()/
 * ime_set_on() — materialise what you need right after the call, which
 * is what ime_view() tells you to do. */

uint32_t ime_view(const ime_t *im);
void     ime_view_clear(ime_t *im);

const char *ime_text(const ime_t *im, size_t *len);    /* after IME_TEXT */
const char *ime_preedit(const ime_t *im, size_t *len);

/* The parts of the preedit, for a renderer that paints the reading, the
   chosen candidate and the okurigana differently. A straight forward of
   skk_preedit_spans() — the composition rule stays in the one place
   that composes. */
int ime_preedit_spans(const ime_t *im, const skk_span_t **out);
int         ime_mode(const ime_t *im);                 /* skk_mode_t */
int         ime_cand_count(const ime_t *im);
int         ime_sel(const ime_t *im);                  /* -1 outside SELECT */
const char *ime_cand(const ime_t *im, int i, size_t *len);

#ifdef __cplusplus
}
#endif
