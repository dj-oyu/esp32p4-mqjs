#include "wav.h"
#include <string.h>

/* little-endian readers (WAV is always LE) */
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

bool wav_parse_mem(const uint8_t *buf, size_t len, wav_info_t *out)
{
    if (!buf || !out || len < 12)
        return false;
    if (memcmp(buf, "RIFF", 4) != 0 || memcmp(buf + 8, "WAVE", 4) != 0)
        return false;

    memset(out, 0, sizeof *out);
    bool have_fmt = false, have_data = false;

    /* Walk chunks after the 12-byte RIFF header. Each chunk: 4-byte id,
       4-byte LE size, then size bytes, padded to an even boundary. */
    size_t pos = 12;
    while (pos + 8 <= len) {
        const uint8_t *id = buf + pos;
        uint32_t csize = rd32(buf + pos + 4);
        size_t body = pos + 8;
        /* clamp: a bogus/streamed size must not run off the buffer */
        size_t avail = len - body;

        if (memcmp(id, "fmt ", 4) == 0 && csize >= 16 && avail >= 16) {
            out->format_tag = rd16(buf + body + 0);
            out->channels = rd16(buf + body + 2);
            out->sample_rate = rd32(buf + body + 4);
            out->bits_per_sample = rd16(buf + body + 14);
            have_fmt = true;
        } else if (memcmp(id, "data", 4) == 0) {
            size_t dsize = csize;
            if (dsize > avail)
                dsize = avail; /* tolerate oversized/placeholder data size */
            out->pcm = buf + body;
            out->pcm_bytes = dsize;
            have_data = true;
            /* data is usually last and may be huge; stop once we have
               both (fmt always precedes data in well-formed files) */
            if (have_fmt)
                break;
        }

        /* advance past body + RIFF even-byte padding, guarding overflow */
        size_t step = csize + (csize & 1u);
        if (step + 8 < step) /* overflow */
            break;
        pos = body + step;
    }

    return have_fmt && have_data && out->channels > 0 &&
           out->sample_rate > 0;
}
