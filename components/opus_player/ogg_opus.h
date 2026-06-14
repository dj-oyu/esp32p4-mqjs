#pragma once

#include <stddef.h>
#include <stdint.h>

#define OGG_OPUS_MAX_PACKET 8192
#define OPUS_AUDIO_MAX_PACKET 1275

typedef struct {
    const uint8_t *data;
    size_t len;
    size_t next_page;
    size_t body_pos;
    size_t page_end;
    const uint8_t *laces;
    uint8_t lace_count;
    uint8_t lace_index;
    uint32_t serial;
    uint8_t packet[OGG_OPUS_MAX_PACKET];
    size_t packet_len;
    unsigned packet_index;
    int error;
} ogg_opus_reader_t;

typedef struct {
    uint32_t sample_rate;
    uint16_t pre_skip;
    uint8_t channels;
} ogg_opus_info_t;

/* Returns 0 on success. Only mapping-family 0 mono/stereo streams are
 * accepted because the initial player uses the single-stream decoder. */
int ogg_opus_open(ogg_opus_reader_t *reader, const uint8_t *data, size_t len,
                  ogg_opus_info_t *info);

/* Returns 1 with the next audio packet, 0 at end of stream, -1 on error. */
int ogg_opus_next(ogg_opus_reader_t *reader, const uint8_t **packet,
                  size_t *packet_len);
