/*
 * skk_core end-to-end host test: the state machine driven through a REAL
 * dictionary image.
 *
 * This layer exists because neither sibling test covers it, and the gap
 * let a silent wrong-answer bug ship: test_skk_kana.c drives the state
 * machine against a stub dictionary (so it never sees a real lookup),
 * and test_skk_dict.c drives the index without a state machine (so it
 * never sees how a key is assembled). The okurigana stem is assembled by
 * one and consumed by the other, so a mistake there is invisible to both.
 *
 * The bug it was written for: okuri_start() took the okurigana stem from
 * the pending romaji unconditionally, so a pending hatsuon "n" was stolen
 * from the reading. "KanJiru" keyed on "かn" instead of "かんj" — and
 * because "かn" EXISTS in SKK-JISYO.L (8 candidates), the lookup
 * succeeded on the wrong entry and committed 兼んじる for 感じる. No
 * crash, no error code. Every ん-final stem was affected.
 *
 * Every expectation below was taken FROM the dictionary, not invented:
 * an "obvious" test word that simply has no entry produces a false
 * failure and wastes a debugging cycle (it did, twice, while writing
 * this).
 *
 * Build/run (host). Needs an image built by tools/skk_prep.py:
 *   python tools/skk_prep.py build SKK-JISYO.L -o /tmp/skk_dict_L.bin
 *   gcc -O1 -g -fsanitize=address -std=c99 -Wall -Wextra \
 *       -I components/skk_core/include \
 *       tools/test_skk_e2e.c components/skk_core/skk_kana.c \
 *       components/skk_core/skk_dict.c -o /tmp/test_skk_e2e
 *   /tmp/test_skk_e2e /tmp/skk_dict_L.bin
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "skk_core.h"

static skk_t      S;
static skk_dict_t D;

/* Feed one byte at a time and concatenate everything that commits, then
   close the span with Enter. This is exactly what an app does. */
static void run(const char *keys, char *out, size_t outsz)
{
    size_t o = 0;
    out[0] = '\0';
    skk_init(&S);
    skk_attach(&S, &D);
    skk_enable(&S, true);

    for (const char *p = keys; *p; p++) {
        uint32_t st = skk_key(&S, p, 1);
        if (st & SKK_ST_COMMIT) {
            size_t n = 0;
            const char *c = skk_commit(&S, &n);
            if (c && o + n < outsz) { memcpy(out + o, c, n); o += n; out[o] = '\0'; }
        }
    }
    uint32_t st = skk_key(&S, "\n", 1);
    if (st & SKK_ST_COMMIT) {
        size_t n = 0;
        const char *c = skk_commit(&S, &n);
        if (c && o + n < outsz) { memcpy(out + o, c, n); o += n; out[o] = '\0'; }
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <skk_dict_L.bin>\n", argv[0]);
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)sz);
    if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        perror("read");
        return 2;
    }
    fclose(f);

    skk_blob_t image = { buf, (size_t)sz };
    int rc = skk_dict_open(&D, image, 0);
    if (rc != 0) { fprintf(stderr, "skk_dict_open: %d\n", rc); return 2; }

    static const struct { const char *keys, *want, *why; } T[] = {
        /* --- hatsuon before the okurigana: the regression --- */
        { "KanJiru",   "感じる", "かんj  (was かn -> 兼んじる)" },
        { "SinJiru",   "信じる", "しんj  (was しn -> 死んじる)" },
        { "SinZuru",   "信ずる", "しんz" },
        { "RonJiru",   "論じる", "ろんj" },
        { "MenJiru",   "免じる", "めんj" },
        { "KanGuru",   "勘ぐる", "かんg" },
        { "YorokonDe", "喜んで", "よろこんd /喜ん/ - the candidate carries the ん too" },

        /* --- control: an INCOMPLETE consonant really is the stem --- */
        { "MotTe",     "持って", "もt  - sokuon builds before the lookup fires" },
        { "TabeRu",    "食べる", "たべr" },
        { "OmoU",      "思う",   "おもu" },
        { "KangaEru",  "考える", "かんがe - ん inside the reading, stem is e" },

        /* --- okuri-nasi still works --- */
        { "Kanji ",    "漢字",   "かん + じ, Space converts" },
    };

    int bad = 0;
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++) {
        char got[512];
        run(T[i].keys, got, sizeof got);
        int ok = strcmp(got, T[i].want) == 0;
        if (!ok) bad++;
        printf("%-4s %-11s -> %-12s (want %-8s)  %s\n",
               ok ? "ok" : "FAIL", T[i].keys, got, T[i].want, T[i].why);
    }

    /* TAB completion against the real index (§6.8). The engine test uses
       a fake dictionary in insertion order; what can only be checked
       here is that the matches come out in the dictionary's own sort
       order, are all genuine extensions of the prefix, and that the
       prefix itself is never offered back. */
    {
        static const char *pfx = "かん";
        const char *prev = NULL;
        size_t prevlen = 0;
        int n = 0, ordered = 1, extends = 1;

        for (size_t i = 0; i < 12; i++) {
            const char *r = NULL;
            size_t rlen = 0;
            if (skk_complete(&D, SKK_BLK_NASI, pfx, strlen(pfx), i,
                             &r, &rlen, NULL) != 1) {
                break;
            }
            n++;
            if (rlen <= strlen(pfx) || memcmp(r, pfx, strlen(pfx)) != 0)
                extends = 0;
            if (prev) {
                size_t m = prevlen < rlen ? prevlen : rlen;
                int c = memcmp(prev, r, m);
                if (c > 0 || (c == 0 && prevlen >= rlen)) ordered = 0;
            }
            prev = r; prevlen = rlen;
        }
        printf("\ncompletion: \"%s\" -> %d matches (first %d shown)\n", pfx, n, n);
        if (n < 3)  { printf("FAIL too few completions for %s\n", pfx); bad++; }
        if (!extends) { printf("FAIL a completion did not extend the prefix\n"); bad++; }
        if (!ordered) { printf("FAIL completions came back out of order\n"); bad++; }
        if (n >= 3 && extends && ordered)
            printf("ok   completion: %d matches, all extend \"%s\", ascending\n",
                   n, pfx);

        /* A reading that nothing extends must report none rather than
           returning the entry itself. */
        {
            const char *r = NULL;
            size_t rlen = 0;
            int rc = skk_complete(&D, SKK_BLK_NASI, "かん", strlen("かん"),
                                  9999, &r, &rlen, NULL);
            if (rc != 0) { printf("FAIL nth past the end must report 0\n"); bad++; }
            else printf("ok   completion: nth past the end reports none\n");
        }
    }

    /* Learning, end to end against the real dictionary and the real
     * state machine (S7). test_skk_kana.c checks WHEN the engine learns
     * with a stub; only here can the loop be closed — pick a candidate
     * that is not first, and see it come back first, through a save and
     * a reload of the file the runtime writes to littlefs. */
    {
        static skk_mru_t M;
        char first_before[128] = "", first_after[128] = "", first_reload[128] = "";
        char save[SKK_MRU_SAVE_MAX];
        size_t slen = 0;
        int nth = 2;                 /* 3rd candidate: not one that is already first */

        skk_mru_init(&M);
        skk_init(&S);
        skk_attach(&S, &D);
        skk_enable(&S, true);

        for (const char *p = "Kanji "; *p; p++) (void)skk_key(&S, p, 1);
        int have = skk_cand_count(&S);
        if (have <= nth) nth = have - 1;
        {
            size_t n = 0;
            const char *c = skk_cand(&S, 0, &n);
            if (c && n < sizeof first_before) memcpy(first_before, c, n);
        }
        /* walk to the nth candidate and commit it */
        for (int i = 0; i < nth; i++) (void)skk_key(&S, " ", 1);
        char chosen[128] = "";
        {
            size_t n = 0;
            const char *c = skk_cand(&S, skk_sel(&S), &n);
            if (c && n < sizeof chosen) memcpy(chosen, c, n);
        }
        (void)skk_key(&S, "\n", 1);

        if (M.n != 0) {
            printf("FAIL learning happened without skk_attach_mru()\n");
            bad++;
        }

        /* Now with the store attached, do it again and re-convert. */
        skk_init(&S);
        skk_attach(&S, &D);
        skk_attach_mru(&S, &M);
        skk_enable(&S, true);
        for (const char *p = "Kanji "; *p; p++) (void)skk_key(&S, p, 1);
        for (int i = 0; i < nth; i++) (void)skk_key(&S, " ", 1);
        (void)skk_key(&S, "\n", 1);

        skk_reset(&S);
        for (const char *p = "Kanji "; *p; p++) (void)skk_key(&S, p, 1);
        {
            size_t n = 0;
            const char *c = skk_cand(&S, 0, &n);
            if (c && n < sizeof first_after) memcpy(first_after, c, n);
        }
        (void)skk_key(&S, "\x07", 1);

        /* Round trip through the bytes the runtime writes to littlefs. */
        skk_mru_t M2;
        if (skk_mru_save(&M, save, sizeof save, &slen) != SKK_OK ||
            skk_mru_load(&M2, save, slen) != SKK_OK) {
            printf("FAIL personal dictionary did not survive save/load\n");
            bad++;
        }
        skk_init(&S);
        skk_attach(&S, &D);
        skk_attach_mru(&S, &M2);
        skk_enable(&S, true);
        for (const char *p = "Kanji "; *p; p++) (void)skk_key(&S, p, 1);
        {
            size_t n = 0;
            const char *c = skk_cand(&S, 0, &n);
            if (c && n < sizeof first_reload) memcpy(first_reload, c, n);
        }

        printf("\nlearning: かんじ 1st was %s, chose %s (#%d)\n",
               first_before, chosen, nth + 1);
        if (nth <= 0 || strcmp(chosen, first_before) == 0) {
            printf("FAIL the test picked a candidate that was already first\n");
            bad++;
        } else if (strcmp(first_after, chosen) != 0) {
            printf("FAIL after learning, 1st is %s, want %s\n",
                   first_after, chosen);
            bad++;
        } else if (strcmp(first_reload, chosen) != 0) {
            printf("FAIL after save+load, 1st is %s, want %s — learning does "
                   "not survive a reboot\n", first_reload, chosen);
            bad++;
        } else {
            printf("ok   learning: 1st is now %s, and still is after "
                   "save+load (%zu B)\n", first_reload, slen);
        }
    }

    /* The counters must actually fill in. convert() used to call
       skk_lookup() rather than skk_lookup_stats(), which left probes,
       fullcmp and dropped at zero forever — the stats reported a
       working IME with no measurable cost, and design §8.1's "measure
       the probes before reconsidering predictive conversion" had no
       instrument behind it. */
    skk_init(&S);
    skk_attach(&S, &D);
    skk_enable(&S, true);
    static const char *drive = "KanJiru RonJiru Kanji ";
    for (const char *p = drive; *p; p++)
        (void)skk_key(&S, p, 1);

    skk_stats_t st;
    skk_stats(&S, &st);
    printf("\nstats: keys=%u consumed=%u lookups=%u probes=%u fullcmp=%u "
           "cands=%u dropped=%u\n",
           st.keys, st.consumed, st.lookups, st.probes, st.fullcmp,
           st.cands, st.dropped);

    int inst = 0;
    if (st.lookups == 0) { printf("FAIL no lookups counted\n"); inst++; }
    if (st.probes == 0)  { printf("FAIL probes stayed 0 (skk_lookup_stats "
                                  "not wired into convert())\n"); inst++; }
    if (st.cands == 0)   { printf("FAIL cands stayed 0\n"); inst++; }
    /* A probe is one 64-byte line the index search touches. With the
       sampled tree (skk_core.h) that is one per level plus one — 5 or 6
       for a real dictionary, where bisection was 12-17. Bound it by what
       THIS image says rather than by a constant, so the check still means
       something when the dictionary changes size or is built without a
       tree. */
    uint32_t lo_p = 8, hi_p = 24;               /* bisecting: ~log2(N) */
    if (D.blk[SKK_BLK_NASI].levels && D.blk[SKK_BLK_ARI].levels) {
        lo_p = D.blk[SKK_BLK_ARI].levels + 1u;  /* the smaller block */
        hi_p = D.blk[SKK_BLK_NASI].levels + 1u;
    }
    if (st.probes && st.lookups &&
        (st.probes / st.lookups < lo_p || st.probes / st.lookups > hi_p)) {
        printf("FAIL probes/lookup = %u, expected %u-%u\n",
               st.probes / st.lookups, lo_p, hi_p);
        inst++;
    }
    if (!inst)
        printf("ok   instrumentation: %u probes over %u lookups (%u lines/"
               "lookup, tree levels %u nasi / %u ari)\n",
               st.probes, st.lookups, st.probes / st.lookups,
               D.blk[SKK_BLK_NASI].levels, D.blk[SKK_BLK_ARI].levels);
    bad += inst;

    size_t n = sizeof T / sizeof T[0];
    printf("\n%s: %zu/%zu conversions, instrumentation %s\n",
           bad ? "FAILED" : "PASS", n - (size_t)(bad - inst), n,
           inst ? "BROKEN" : "ok");
    free(buf);
    return bad ? 1 : 0;
}
