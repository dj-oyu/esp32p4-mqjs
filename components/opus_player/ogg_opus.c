#include "ogg_opus.h"

#include <stdbool.h>
#include <string.h>

static uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int next_page(ogg_opus_reader_t *r)
{
    if (r->next_page == r->len)
        return 0;
    if (r->len - r->next_page < 27)
        return -1;

    const uint8_t *h = r->data + r->next_page;
    if (memcmp(h, "OggS", 4) != 0 || h[4] != 0)
        return -1;
    bool continuation = (h[5] & 0x01) != 0;
    if (continuation != (r->packet_len != 0))
        return -1;

    uint8_t lace_count = h[26];
    if (r->len - r->next_page < (size_t)27 + lace_count)
        return -1;

    const uint8_t *laces = h + 27;
    size_t body_len = 0;
    for (unsigned i = 0; i < lace_count; i++)
        body_len += laces[i];
    size_t page_len = 27 + (size_t)lace_count + body_len;
    if (page_len > r->len - r->next_page)
        return -1;

    uint32_t serial = read_le32(h + 14);
    if (r->serial && serial != r->serial)
        return -1;
    r->serial = serial;
    r->laces = laces;
    r->lace_count = lace_count;
    r->lace_index = 0;
    r->body_pos = r->next_page + 27 + lace_count;
    r->page_end = r->next_page + page_len;
    r->next_page = r->page_end;
    return 1;
}

static int next_packet(ogg_opus_reader_t *r, const uint8_t **packet,
                       size_t *packet_len)
{
    for (;;) {
        if (r->lace_index == r->lace_count) {
            int page = next_page(r);
            if (page <= 0)
                return page;
        }

        uint8_t part_len = r->laces[r->lace_index++];
        if (part_len > r->page_end - r->body_pos ||
            part_len > sizeof(r->packet) - r->packet_len)
            return -1;
        memcpy(r->packet + r->packet_len, r->data + r->body_pos, part_len);
        r->body_pos += part_len;
        r->packet_len += part_len;

        if (part_len < 255) {
            *packet = r->packet;
            *packet_len = r->packet_len;
            r->packet_len = 0;
            r->packet_index++;
            return 1;
        }
    }
}

int ogg_opus_open(ogg_opus_reader_t *r, const uint8_t *data, size_t len,
                  ogg_opus_info_t *info)
{
    if (!r || !data || !info)
        return -1;
    memset(r, 0, sizeof(*r));
    r->data = data;
    r->len = len;

    const uint8_t *packet = NULL;
    size_t packet_len = 0;
    if (next_packet(r, &packet, &packet_len) != 1 || packet_len < 19 ||
        memcmp(packet, "OpusHead", 8) != 0 || packet[8] > 15 ||
        packet[9] < 1 || packet[9] > 2 || packet[18] != 0)
        return -1;

    info->sample_rate = 48000;
    info->channels = packet[9];
    info->pre_skip = read_le16(packet + 10);

    if (next_packet(r, &packet, &packet_len) != 1 || packet_len < 8 ||
        memcmp(packet, "OpusTags", 8) != 0)
        return -1;
    return 0;
}

int ogg_opus_next(ogg_opus_reader_t *r, const uint8_t **packet,
                  size_t *packet_len)
{
    if (!r || !packet || !packet_len || r->error)
        return -1;
    int ret = next_packet(r, packet, packet_len);
    if (ret == 1 && (*packet_len == 0 || *packet_len > OPUS_AUDIO_MAX_PACKET))
        ret = -1;
    if (ret < 0)
        r->error = 1;
    return ret;
}
