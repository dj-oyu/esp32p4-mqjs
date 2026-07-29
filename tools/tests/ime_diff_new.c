/* NEW-PATH trace: the same key script through ime_core, with the same real
   dictionary and a learning dictionary attached (the JS binding attaches one
   at mqjs_runtime.c:5707, so leaving it off here would change candidate order
   and produce a false mismatch). Emits the same lines as ime_diff_old.js.inc.
   Throwaway.
 *
 *   gcc -O2 -std=c99 -I components/skk_core/include -I components/ime_core/include \
 *       ime_diff_new.c components/ime_core/ime_core.c components/skk_core/skk_kana.c \
 *       components/skk_core/skk_dict.c -o /tmp/ime_diff_new
 *   /tmp/ime_diff_new components/skk_core/skk_dict.bin
 */
#include "ime_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const char *k; size_t n; } key_t_;

#define K(s)   { s, sizeof(s) - 1 }
#define TOK(s) { "\0" s, sizeof(s) }   /* NUL + name, no trailing NUL */

static const key_t_ KEYS[] = {
    TOK("ime"),
    K("a"), K("i"),
    K("K"), K("a"), K("n"), K("j"), K("i"),
    K(" "), K(" "), K("\n"),
    K("O"), K("k"), K("u"), K("R"), K("u"), K("\n"),
    K("K"), K("a"), K("n"),
    K("\b"),
    TOK("left"),
    TOK("esc"),
    TOK("up"),
    TOK("ime"),
    K("a"), K("\n"), TOK("up"),
};
#define NKEYS ((int)(sizeof KEYS / sizeof KEYS[0]))

static skk_dict_t D;
static skk_mru_t  MRU;

int main(int argc, char **argv)
{
    ime_t im;
    skk_blob_t image;
    long sz;
    int i;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <skk_dict.bin>\n", argv[0]);
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)sz);
    if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        fprintf(stderr, "read failed\n");
        return 2;
    }
    fclose(f);
    image.base = buf;
    image.len  = (size_t)sz;
    if (skk_dict_open(&D, image, 0) != 0) {
        fprintf(stderr, "skk_dict_open failed\n");
        return 2;
    }

    ime_init(&im);
    skk_mru_init(&MRU);
    ime_attach(&im, &D);
    ime_attach_mru(&im, &MRU);

    printf("DICT ?\n");
    for (i = 0; i < NKEYS; i++) {
        ime_disp_t d = ime_feed(&im, KEYS[i].k, KEYS[i].n);
        const char *cls = d == IME_TEXT ? "TEXT"
                        : (d == IME_PASS ? "PASS" : "TAKEN");
        size_t tn = 0;
        const char *t = d == IME_TEXT ? ime_text(&im, &tn) : NULL;
        printf("T %d %s pre=%s mode=%d nc=%d txt=%.*s\n",
               i, cls, ime_preedit(&im, NULL), ime_mode(&im),
               ime_cand_count(&im), (int)tn, t ? t : "");
    }
    printf("READY %d ERR -\n", ime_ready(&im) ? 1 : 0);
    printf("TRACE END\n");
    return 0;
}
