/* Host reproduction of the on-device QR path: qr_marker_detect (prefilter)
 * + quirc, run over raw 400x300 grayscale corpus frames. Confirms whether
 * the custom finder prefilter gates out frames that quirc alone can decode.
 *
 * Build (WSL):
 *   gcc -O2 -DQUIRC_FLOAT_TYPE=float \
 *     -I components/cam_tab5/include -I components/quirc/include \
 *     components/cam_tab5/tools/qr_device_repro.c \
 *     components/cam_tab5/qr_marker.c \
 *     components/quirc/src/quirc.c components/quirc/src/identify.c \
 *     components/quirc/src/decode.c components/quirc/src/version_db.c \
 *     -lm -o .qrtmp/qr_device_repro
 *
 * Run:  .qrtmp/qr_device_repro 400 300 .qrtmp/gray/*.gray
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "qr_marker.h"
#include "quirc.h"

static uint8_t *load_gray(const char *path, int w, int h)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    size_t n = (size_t)w * h;
    uint8_t *buf = malloc(n);
    if (buf && fread(buf, 1, n, f) != n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    return buf;
}

static const char *base(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s W H file.gray...\n", argv[0]);
        return 2;
    }
    int w = atoi(argv[1]), h = atoi(argv[2]);
    struct quirc *q = quirc_new();
    if (!q || quirc_resize(q, w, h) < 0) {
        fprintf(stderr, "quirc init failed\n");
        return 2;
    }

    printf("%-44s %-6s %-9s %-4s %-5s %-5s %s\n", "frame", "marker", "polarity",
           "mrk", "cand", "decd", "result");
    for (int a = 3; a < argc; a++) {
        uint8_t *gray = load_gray(argv[a], w, h);
        if (!gray) {
            fprintf(stderr, "skip (read fail): %s\n", argv[a]);
            continue;
        }

        /* 1. device prefilter */
        qr_marker_result_t mr = { 0 };
        bool marker_ok = qr_marker_detect(gray, w, h, &mr);

        /* 2. quirc on the SAME gray, independent of the prefilter verdict.
         * Apply the device's inverted-polarity flip when the prefilter says so. */
        int qw, qh;
        uint8_t *qbuf = quirc_begin(q, &qw, &qh);
        for (int i = 0; i < w * h; i++)
            qbuf[i] = (marker_ok && mr.inverted) ? (uint8_t)(255 - gray[i]) : gray[i];
        quirc_end(q);
        int cand = quirc_count(q);

        int decoded = 0;
        for (int i = 0; i < cand; i++) {
            struct quirc_code code;
            struct quirc_data data;
            quirc_extract(q, i, &code);
            quirc_decode_error_t err = quirc_decode(&code, &data);
            if (err == QUIRC_ERROR_DATA_ECC) {
                quirc_flip(&code);
                err = quirc_decode(&code, &data);
            }
            if (err == QUIRC_SUCCESS && data.payload_len > 0)
                decoded++;
        }

        const char *want_ok = strstr(base(argv[a]), "_ok") ? "ok" : "fail";
        const char *gate = marker_ok ? "PASS" : "block";
        /* device returns success only when BOTH the prefilter passes AND quirc decodes */
        const char *device = (marker_ok && decoded) ? "READ" : "miss";
        const char *quirc_alone = decoded ? "READ" : "miss";
        printf("%-44s %-6s %-9c %-4d %-5d %-5d device=%s quirc=%s expect=%s\n",
               base(argv[a]), gate, mr.inverted ? 'I' : 'N', mr.markers, cand,
               decoded, device, quirc_alone, want_ok);
        free(gray);
    }
    quirc_destroy(q);
    return 0;
}
