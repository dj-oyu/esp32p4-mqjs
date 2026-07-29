/*
 * ime_core — see include/ime_core.h for what this layer is and why it
 * exists. This file is the session policy that used to live, three
 * times over, in JS.
 */
#include "ime_core.h"

#include <string.h>

/* The one token this layer owns. Compared with its length because a
   token is NUL-prefixed, so the usual string calls would see "". */
static const char TOK_IME[] = "\0ime";
#define TOK_IME_LEN 4

static bool is_tok(const char *key, size_t len, const char *tok, size_t tlen)
{
    return len == tlen && key[0] == '\0' && memcmp(key, tok, tlen) == 0;
}

void ime_init(ime_t *im)
{
    memset(im, 0, sizeof *im);
    skk_init(&im->skk);
    im->mode = skk_mode(&im->skk);
    im->ncand = 0;
}

void ime_attach(ime_t *im, const skk_dict_t *d)
{
    skk_attach(&im->skk, d);
    im->ready = d != NULL;
    if (!im->ready) {
        /* No dictionary means nothing may be armed: skk_key() would
           consume readings it can never convert, swallowing keys the
           app would otherwise have handled. */
        im->on = false;
        skk_enable(&im->skk, false);
    }
}

void ime_attach_mru(ime_t *im, skk_mru_t *m)
{
    skk_attach_mru(&im->skk, m);
}

bool ime_ready(const ime_t *im) { return im->ready; }
bool ime_on(const ime_t *im) { return im->on; }

void ime_set_on(ime_t *im, bool on)
{
    if (on && !im->ready)
        return;              /* nothing to arm */
    if (im->on == on)
        return;
    im->on = on;
    skk_enable(&im->skk, on);
    if (on) {
        /* C-j is unreachable on this keyboard (kbd_core drops Ctrl+Space
           because NUL collides with the token prefix), so ASCII mode
           would otherwise be a one-way door. */
        skk_set_mode(&im->skk, SKK_MODE_KANA);
    } else {
        skk_reset(&im->skk); /* discards the reading, never commits it */
    }
    im->mode = skk_mode(&im->skk);
    im->ncand = 0;
    im->text = NULL;
    im->text_len = 0;
    im->view |= IME_V_ENABLE | IME_V_PREEDIT | IME_V_CANDS | IME_V_MODE;
}

void ime_reset(ime_t *im)
{
    skk_reset(&im->skk);
    im->mode = skk_mode(&im->skk);
    im->ncand = 0;
    im->text = NULL;
    im->text_len = 0;
    im->view |= IME_V_PREEDIT | IME_V_CANDS | IME_V_MODE;
}

ime_disp_t ime_feed(ime_t *im, const char *key, size_t len)
{
    if (len == 0)
        return IME_PASS;

    /* The toggle is answered whether or not the engine is armed —
       otherwise 「あ」 would be dead exactly when the user is trying to
       turn the IME ON. */
    if (is_tok(key, len, TOK_IME, TOK_IME_LEN)) {
        ime_set_on(im, !im->on);
        return IME_TAKEN;
    }

    im->text = NULL;
    im->text_len = 0;
    if (!im->on || !im->ready)
        return IME_PASS;

    uint32_t st = skk_key(&im->skk, key, len);
    if (st == 0)
        return IME_PASS;     /* the engine did not want it */

    /* Fan the bitmask out. Which accessor to re-read is decided ONLY by
       these bits: entering v sets CANDS once, and the SPACE/x walk
       inside it sets SEL alone, so the candidate list is built once per
       conversion rather than per keystroke (design §4.4). */
    if (st & SKK_ST_COMMIT)
        im->text = skk_commit(&im->skk, &im->text_len);
    if (st & SKK_ST_MODE) {
        im->mode = skk_mode(&im->skk);
        im->view |= IME_V_MODE;
    }
    if (st & (SKK_ST_PREEDIT | SKK_ST_COMMIT))
        im->view |= IME_V_PREEDIT;
    if (st & SKK_ST_CANDS) {
        im->ncand = skk_cand_count(&im->skk);
        im->view |= IME_V_CANDS | IME_V_SEL;
    } else if (st & SKK_ST_SEL) {
        im->view |= IME_V_SEL;
    } else if (im->mode != SKK_MODE_SELECT && im->ncand) {
        /* Left v without the engine saying so (a commit, a cancel): the
           candidate bar is stale and must fold, or it hangs on screen
           over whatever is typed next. */
        im->ncand = 0;
        im->view |= IME_V_CANDS;
    }
    return (st & SKK_ST_COMMIT) ? IME_TEXT : IME_TAKEN;
}

uint32_t ime_view(const ime_t *im) { return im->view; }
void ime_view_clear(ime_t *im) { im->view = 0; }

const char *ime_text(const ime_t *im, size_t *len)
{
    if (len)
        *len = im->text_len;
    return im->text;
}

const char *ime_preedit(const ime_t *im, size_t *len)
{
    return skk_preedit(&im->skk, len);
}

int ime_preedit_spans(const ime_t *im, const skk_span_t **out)
{
    return skk_preedit_spans(&im->skk, out);
}

int ime_mode(const ime_t *im) { return im->mode; }
int ime_cand_count(const ime_t *im) { return im->ncand; }

int ime_sel(const ime_t *im) { return skk_sel(&im->skk); }

const char *ime_cand(const ime_t *im, int i, size_t *len)
{
    return skk_cand(&im->skk, i, len);
}
