/*
 * Host unit test for ime_core — the IME session policy that used to be
 * copied into every app (components/ime_core/ime_core.c).
 *
 * What this checks is NOT conversion (that is skk_core's own tests) but
 * the layer's contract, and above all THE ORDERING GUARANTEE: a caller
 * that feeds every key here first cannot expand "\0left" into "\x1b[D"
 * behind the engine's back. In the JS era that rule lived in a comment
 * and was watched by tools/ssh_vt_imetest.sh (since deleted) for exactly
 * one of the three apps that needed it.
 *
 * The dictionary half (skk_dict.c) is deliberately NOT linked: the
 * lookup is a tiny in-memory fake, so a failure here is a policy
 * failure and nothing else.
 *
 * Build/run (host):
 *   gcc -O2 -Wall -Wextra -std=c99 \
 *       -I components/skk_core/include -I components/ime_core/include \
 *       tools/tests/test_ime_core.c components/ime_core/ime_core.c \
 *       components/skk_core/skk_kana.c \
 *       -o /tmp/test_ime_core && /tmp/test_ime_core
 */
#include "ime_core.h"

#include <stdio.h>
#include <string.h>

static int g_fails;

#define CHECK(name, cond)                                                 \
    do {                                                                  \
        if (cond) {                                                       \
            printf("ok %s\n", (name));                                    \
        } else {                                                          \
            g_fails++;                                                    \
            printf("FAIL %s (line %d)\n", (name), __LINE__);              \
        }                                                                 \
    } while (0)

/* ---- fake dictionary (same shape as tools/test_skk_kana.c) --------- */

#define FK_MAX   8
#define FK_CANDS 4

typedef struct {
    const char *reading;
    int         blk;
    int         n;
    uint32_t    off[FK_CANDS];
    uint16_t    len[FK_CANDS];
} fk_ent_t;

static char       g_blob[512];
static size_t     g_blob_len;
static fk_ent_t   g_fk[FK_MAX];
static int        g_fk_n;
static skk_dict_t g_dict;

static void fk_add(const char *reading, int blk, const char *const *cands)
{
    fk_ent_t *e = &g_fk[g_fk_n++];

    e->reading = reading;
    e->blk     = blk;
    e->n       = 0;
    for (int i = 0; cands[i] && e->n < FK_CANDS; i++) {
        size_t l = strlen(cands[i]);
        memcpy(g_blob + g_blob_len, cands[i], l);
        e->off[e->n] = (uint32_t)g_blob_len;
        e->len[e->n] = (uint16_t)l;
        e->n++;
        g_blob_len += l;
    }
}

static void fk_init(void)
{
    static const char *const kanji[] = { "漢字", "感じ", "幹事", NULL };
    static const char *const okuru[] = { "送", "贈", NULL };

    g_blob_len = 0;
    g_fk_n     = 0;
    fk_add("かんじ", SKK_BLK_NASI, kanji);
    fk_add("おくr", SKK_BLK_ARI, okuru);

    memset(&g_dict, 0, sizeof g_dict);
    g_dict.image.base = (const uint8_t *)g_blob;
    g_dict.image.len  = sizeof g_blob;
    g_dict.text       = (const uint8_t *)g_blob;
    g_dict.text_len   = g_blob_len;
}

int skk_lookup_stats(const skk_dict_t *d, skk_blk_t blk, const char *reading,
                     size_t len, skk_cand_t *out, size_t cap, size_t *out_n,
                     skk_stats_t *st)
{
    (void)d;
    if (out_n) *out_n = 0;
    if (st) st->lookups++;
    for (int i = 0; i < g_fk_n; i++) {
        size_t n = 0;
        if (g_fk[i].blk != (int)blk) continue;
        if (strlen(g_fk[i].reading) != len) continue;
        if (memcmp(g_fk[i].reading, reading, len) != 0) continue;
        for (int j = 0; j < g_fk[i].n && n < cap; j++) {
            out[n].off = g_fk[i].off[j];
            out[n].len = g_fk[i].len[j];
            out[n].src = SKK_SRC_DICT;
            n++;
            if (st) st->cands++;
        }
        if (out_n) *out_n = n;
        return SKK_OK;
    }
    return SKK_OK;
}

int skk_lookup(const skk_dict_t *d, skk_blk_t blk, const char *reading,
               size_t len, skk_cand_t *out, size_t cap, size_t *out_n)
{
    return skk_lookup_stats(d, blk, reading, len, out, cap, out_n, NULL);
}

int skk_complete(const skk_dict_t *d, skk_blk_t blk, const char *prefix,
                 size_t plen, size_t nth, const char **out, size_t *out_len,
                 uint32_t *probes)
{
    (void)d; (void)blk; (void)prefix; (void)plen; (void)nth;
    (void)out; (void)out_len; (void)probes;
    return 0;
}

const char *skk_cand_text(const skk_dict_t *d, const skk_cand_t *c, size_t *len)
{
    if (len) *len = c->len;
    return (const char *)d->image.base + c->off;
}

int skk_mru_note(skk_mru_t *m, skk_blk_t blk, const char *reading, size_t len,
                 const char *text, size_t tlen)
{
    (void)m; (void)blk; (void)reading; (void)len; (void)text; (void)tlen;
    return SKK_OK;
}

int skk_mru_apply(const skk_mru_t *m, const skk_dict_t *d, skk_blk_t blk,
                  const char *reading, size_t len, skk_cand_t *cands,
                  size_t n, size_t cap, size_t *out_n)
{
    (void)m; (void)d; (void)blk; (void)reading; (void)len; (void)cands;
    (void)cap;
    if (out_n) *out_n = n;
    return SKK_OK;
}

const char *skk_mru_text(const skk_mru_t *m, const skk_cand_t *c, size_t *len)
{
    (void)m;
    if (len) *len = c->len;
    return NULL;
}

/* ---- helpers ------------------------------------------------------- */

static ime_disp_t feed1(ime_t *im, const char *k)
{
    return ime_feed(im, k, strlen(k));
}

/* A "\0name" token: strlen() would see "" so the length is explicit. */
static ime_disp_t feedtok(ime_t *im, const char *name)
{
    char buf[16];
    size_t n = strlen(name);
    buf[0] = '\0';
    memcpy(buf + 1, name, n);
    return ime_feed(im, buf, n + 1);
}

/* Feed a run of keys the way a real caller does: every IME_TEXT along
   the way is appended to the sink, because plain kana commits one
   character at a time rather than accumulating in the preedit. */
static char g_sink[256];

static void sink_clear(void) { g_sink[0] = '\0'; }

static ime_disp_t feedstr(ime_t *im, const char *s)
{
    ime_disp_t d = IME_PASS;
    for (const char *p = s; *p; p++) {
        char c = *p;
        d = ime_feed(im, &c, 1);
        if (d == IME_TEXT) {
            size_t n = 0;
            const char *t = ime_text(im, &n);
            strncat(g_sink, t, n);
        }
    }
    return d;
}

static bool sink_is(const char *want)
{
    return strcmp(g_sink, want) == 0;
}

static bool text_is(const ime_t *im, const char *want)
{
    size_t n = 0;
    const char *t = ime_text(im, &n);
    return t && n == strlen(want) && memcmp(t, want, n) == 0;
}

int main(void)
{
    ime_t im;

    fk_init();

    /* ---- without a dictionary nothing may be armed or swallowed ---- */
    ime_init(&im);
    CHECK("no-dict: not ready", !ime_ready(&im) && !ime_on(&im));
    CHECK("no-dict: toggle cannot arm",
          (feedtok(&im, "ime") == IME_TAKEN) && !ime_on(&im));
    CHECK("no-dict: keys pass through", feed1(&im, "a") == IME_PASS);

    /* ---- armed ----------------------------------------------------- */
    ime_attach(&im, &g_dict);
    CHECK("attach: ready but not armed", ime_ready(&im) && !ime_on(&im));
    CHECK("off: keys pass through", feed1(&im, "a") == IME_PASS);

    CHECK("toggle on", feedtok(&im, "ime") == IME_TAKEN && ime_on(&im));
    CHECK("toggle raises ENABLE", (ime_view(&im) & IME_V_ENABLE) != 0);
    ime_view_clear(&im);

    /* plain kana commits one character at a time as the romaji lands,
       so nothing accumulates in the preedit (that is ~ mode's job) */
    sink_clear();
    CHECK("kana commits", feedstr(&im, "ai") == IME_TEXT);
    CHECK("kana text", sink_is("あい") && text_is(&im, "い"));
    CHECK("kana view", (ime_view(&im) & IME_V_PREEDIT) != 0);
    ime_view_clear(&im);

    /* ---- ~ -> v -> commit ------------------------------------------ */
    CHECK("midashi taken", feedstr(&im, "Kanji") == IME_TAKEN);
    CHECK("midashi mode", ime_mode(&im) == SKK_MODE_MIDASHI);
    CHECK("midashi has preedit", strlen(ime_preedit(&im, NULL)) > 0);
    CHECK("midashi emits nothing", ime_text(&im, NULL) == NULL);
    ime_view_clear(&im);

    CHECK("select taken", feed1(&im, " ") == IME_TAKEN);
    CHECK("select mode", ime_mode(&im) == SKK_MODE_SELECT);
    CHECK("select has cands", ime_cand_count(&im) == 3);
    CHECK("select raises CANDS", (ime_view(&im) & IME_V_CANDS) != 0);
    ime_view_clear(&im);

    /* the walk inside v moves the selection only: the candidate list is
       built once per conversion, not once per keystroke (design §4.4) */
    int n_before = ime_cand_count(&im);
    CHECK("walk taken", feed1(&im, " ") == IME_TAKEN);
    CHECK("walk keeps the list", ime_cand_count(&im) == n_before);
    CHECK("walk raises SEL only",
          (ime_view(&im) & IME_V_SEL) && !(ime_view(&im) & IME_V_CANDS));
    CHECK("walk moved the selection", ime_sel(&im) == 1);
    ime_view_clear(&im);

    CHECK("commit", feed1(&im, "\n") == IME_TEXT);
    CHECK("commit text is the selection", text_is(&im, "感じ"));
    CHECK("commit folds the candidates", ime_cand_count(&im) == 0);
    CHECK("commit clears the preedit", strlen(ime_preedit(&im, NULL)) == 0);
    ime_view_clear(&im);

    /* ---- THE ORDERING GUARANTEE ------------------------------------
       While a reading is open the engine takes the arrows and ESC. If a
       caller expanded tokens before asking, the cursor would walk away
       from a preedit drawn at it and "\x1b[D" would leak to the wire. */
    /* the okurigana completes the reading and converts on its own, so
       this lands in v — the commit still needs a key */
    CHECK("okuri converts", feedstr(&im, "OkuRu") == IME_TAKEN);
    CHECK("okuri reached select", ime_mode(&im) == SKK_MODE_SELECT);
    CHECK("okuri commits", feed1(&im, "\n") == IME_TEXT);
    CHECK("okuri text", text_is(&im, "送る"));
    ime_view_clear(&im);

    CHECK("re-open a reading", feedstr(&im, "Kan") == IME_TAKEN);
    CHECK("arrow is swallowed while ~ is open",
          feedtok(&im, "left") == IME_TAKEN);
    CHECK("arrow left the reading alone", strlen(ime_preedit(&im, NULL)) > 0);
    CHECK("arrow emitted nothing", ime_text(&im, NULL) == NULL);

    CHECK("esc cancels the reading", feedtok(&im, "esc") == IME_TAKEN);
    CHECK("esc discarded it", strlen(ime_preedit(&im, NULL)) == 0);
    CHECK("esc committed nothing", ime_text(&im, NULL) == NULL);

    /* ...and with nothing open they are the caller's again, or ESC
       could never reach the app that wants to close a screen with it */
    CHECK("arrow passes when idle", feedtok(&im, "left") == IME_PASS);
    CHECK("esc passes when idle", feedtok(&im, "esc") == IME_PASS);
    CHECK("unknown token passes", feedtok(&im, "rotate") == IME_PASS);

    /* ---- turning it off discards, never commits -------------------- */
    CHECK("reading open again", feedstr(&im, "Kanji") == IME_TAKEN);
    ime_view_clear(&im);
    CHECK("toggle off", feedtok(&im, "ime") == IME_TAKEN && !ime_on(&im));
    CHECK("toggle off emitted nothing", ime_text(&im, NULL) == NULL);
    CHECK("toggle off discarded the reading",
          strlen(ime_preedit(&im, NULL)) == 0);
    CHECK("toggle off raises ENABLE", (ime_view(&im) & IME_V_ENABLE) != 0);
    CHECK("off: keys pass through again", feed1(&im, "a") == IME_PASS);

    /* ---- reset drops a pending reading without committing ---------- */
    feedtok(&im, "ime");
    CHECK("armed again", ime_on(&im));
    feedstr(&im, "Kanji");
    ime_reset(&im);
    CHECK("reset cleared the preedit", strlen(ime_preedit(&im, NULL)) == 0);
    CHECK("reset committed nothing", ime_text(&im, NULL) == NULL);

    /* ---- detaching disarms ----------------------------------------- */
    ime_attach(&im, NULL);
    CHECK("detach disarms", !ime_ready(&im) && !ime_on(&im));
    CHECK("detached keys pass through", feed1(&im, "a") == IME_PASS);

    if (g_fails)
        printf("ime_core selftest: FAIL x%d\n", g_fails);
    else
        printf("ime_core selftest: ALL PASS\n");
    return g_fails ? 1 : 0;
}
