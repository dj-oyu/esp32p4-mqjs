/*
 * Minimal RIFF/WAVE PCM parser. Pure C, no ESP/IDF deps — host-testable.
 *
 * Parses a WAV held entirely in memory (an embedded blob or a fully read
 * file), walking the chunk list to locate "fmt " and "data" while
 * skipping anything else (LIST/INFO, fact, ...). Only uncompressed
 * integer PCM (format tag 1) is recognised; the player downstream only
 * handles 16-bit, but the parser reports bits_per_sample so the caller
 * can reject other depths with a clear message.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits_per_sample;
    uint16_t format_tag;   /* 1 = PCM; others unsupported */
    const uint8_t *pcm;    /* points into the input buffer (data chunk) */
    size_t pcm_bytes;      /* data chunk length, clamped to the buffer */
} wav_info_t;

/* Returns true and fills *out on a valid RIFF/WAVE with a fmt + data
 * chunk. out->pcm aliases the input buffer (no copy); the buffer must
 * outlive use. Tolerates a data size larger than the buffer (clamped)
 * and odd chunk sizes (RIFF word padding). */
bool wav_parse_mem(const uint8_t *buf, size_t len, wav_info_t *out);
