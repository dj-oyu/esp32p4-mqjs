/*
 * Host unit test for the WAV parser. Build + run in WSL:
 *   cd components/audio_tab5
 *   gcc -O2 -I. -fsanitize=address -o /tmp/wav_test tools/wav_test.c wav.c
 *   /tmp/wav_test ../../../../assets/audio/tab5-boot.wav
 * (the path arg is optional; synthetic cases always run)
 */
#include "wav.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

static int failures;
#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("FAIL: %s\n", msg);                                       \
            failures++;                                                      \
        }                                                                    \
    } while (0)

/* build a minimal RIFF/WAVE in a caller buffer; returns total length */
static size_t build_wav(uint8_t *b, uint16_t ch, uint32_t rate, uint16_t bits,
                        size_t data_bytes, int with_list)
{
    size_t p = 0;
    uint32_t byte_rate = rate * ch * (bits / 8);
    uint16_t align = (uint16_t)(ch * (bits / 8));
    memcpy(b + p, "RIFF", 4); p += 4;
    size_t riff_size_pos = p; p += 4;            /* fill later */
    memcpy(b + p, "WAVE", 4); p += 4;
    memcpy(b + p, "fmt ", 4); p += 4;
    b[p]=16;b[p+1]=0;b[p+2]=0;b[p+3]=0; p += 4;  /* fmt size */
    b[p]=1;b[p+1]=0; p += 2;                      /* PCM */
    b[p]=ch&0xff;b[p+1]=ch>>8; p += 2;
    b[p]=rate&0xff;b[p+1]=(rate>>8)&0xff;b[p+2]=(rate>>16)&0xff;b[p+3]=(rate>>24)&0xff; p+=4;
    b[p]=byte_rate&0xff;b[p+1]=(byte_rate>>8)&0xff;b[p+2]=(byte_rate>>16)&0xff;b[p+3]=(byte_rate>>24)&0xff; p+=4;
    b[p]=align&0xff;b[p+1]=align>>8; p += 2;
    b[p]=bits&0xff;b[p+1]=bits>>8; p += 2;
    if (with_list) {                              /* an odd-sized LIST to skip */
        memcpy(b + p, "LIST", 4); p += 4;
        uint32_t ls = 5;                          /* odd -> tests padding */
        b[p]=ls&0xff;b[p+1]=0;b[p+2]=0;b[p+3]=0; p += 4;
        memcpy(b + p, "INFOx", 5); p += 5;
        b[p++] = 0;                               /* pad byte */
    }
    memcpy(b + p, "data", 4); p += 4;
    b[p]=data_bytes&0xff;b[p+1]=(data_bytes>>8)&0xff;b[p+2]=(data_bytes>>16)&0xff;b[p+3]=(data_bytes>>24)&0xff; p += 4;
    for (size_t i = 0; i < data_bytes; i++)
        b[p + i] = (uint8_t)i;
    p += data_bytes;
    uint32_t riff = (uint32_t)(p - 8);
    b[riff_size_pos]=riff&0xff;b[riff_size_pos+1]=(riff>>8)&0xff;
    b[riff_size_pos+2]=(riff>>16)&0xff;b[riff_size_pos+3]=(riff>>24)&0xff;
    return p;
}

int main(int argc, char **argv)
{
    uint8_t buf[1024];
    wav_info_t w;

    /* 1: plain 48k stereo 16-bit, no LIST */
    size_t n = build_wav(buf, 2, 48000, 16, 40, 0);
    CHECK(wav_parse_mem(buf, n, &w), "plain parse");
    CHECK(w.channels == 2 && w.sample_rate == 48000 && w.bits_per_sample == 16,
          "plain fmt");
    CHECK(w.pcm_bytes == 40 && w.pcm[0] == 0 && w.pcm[1] == 1, "plain data");

    /* 2: with an odd-sized LIST chunk before data (the real file's shape) */
    n = build_wav(buf, 2, 48000, 16, 40, 1);
    CHECK(wav_parse_mem(buf, n, &w), "LIST-skip parse");
    CHECK(w.pcm_bytes == 40 && w.pcm[1] == 1, "LIST-skip data offset");

    /* 3: mono 16k */
    n = build_wav(buf, 1, 16000, 16, 32, 0);
    CHECK(wav_parse_mem(buf, n, &w) && w.channels == 1 && w.sample_rate == 16000,
          "mono 16k");

    /* 4: oversized data size clamps to buffer, no OOB read (ASAN) */
    n = build_wav(buf, 2, 48000, 16, 16, 0);
    buf[n - 16 - 1] = 0; /* leave structure; now corrupt the data size big */
    {
        /* rewrite the data chunk size field to 0xFFFFFFFF */
        /* locate "data": it's 16 bytes of pcm back from end => size at n-16-4 */
        size_t szpos = n - 16 - 4;
        buf[szpos] = 0xff; buf[szpos+1] = 0xff; buf[szpos+2] = 0xff; buf[szpos+3] = 0xff;
        CHECK(wav_parse_mem(buf, n, &w), "oversized-data parse");
        CHECK(w.pcm_bytes == 16, "oversized-data clamped");
    }

    /* 5: garbage rejected */
    CHECK(!wav_parse_mem((const uint8_t *)"not a wav!!!!", 13, &w), "reject garbage");
    CHECK(!wav_parse_mem(buf, 4, &w), "reject too-short");

    /* 6: the real asset, if given */
    if (argc >= 2) {
        FILE *f = fopen(argv[1], "rb");
        if (!f) { perror(argv[1]); return 2; }
        fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
        uint8_t *data = malloc(len);
        if (fread(data, 1, len, f) != (size_t)len) { perror("read"); return 2; }
        fclose(f);
        CHECK(wav_parse_mem(data, len, &w), "real file parse");
        printf("real: %u Hz, %u ch, %u-bit, fmt=%u, %zu PCM bytes (%.2fs), "
               "data offset=%ld\n",
               w.sample_rate, w.channels, w.bits_per_sample, w.format_tag,
               w.pcm_bytes,
               (double)w.pcm_bytes / (w.sample_rate * w.channels * (w.bits_per_sample / 8)),
               (long)(w.pcm - data));
        CHECK(w.bits_per_sample == 16, "real is 16-bit (audio_tab5 needs it)");
        free(data);
    }

    printf(failures ? "\n%d FAILED\n" : "\nALL PASS\n", failures);
    return failures ? 1 : 0;
}
