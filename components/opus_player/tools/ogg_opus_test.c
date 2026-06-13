#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "../ogg_opus.h"

int main(int argc, char **argv)
{
    assert(argc == 2);
    FILE *f = fopen(argv[1], "rb");
    assert(f);
    assert(fseek(f, 0, SEEK_END) == 0);
    long size = ftell(f);
    assert(size > 0);
    rewind(f);

    uint8_t *data = malloc((size_t)size);
    assert(data);
    assert(fread(data, 1, (size_t)size, f) == (size_t)size);
    fclose(f);

    ogg_opus_reader_t reader;
    ogg_opus_info_t info;
    assert(ogg_opus_open(&reader, data, (size_t)size, &info) == 0);
    assert(info.sample_rate == 48000);
    assert(info.channels == 2);

    unsigned packets = 0;
    const uint8_t *packet;
    size_t packet_len;
    int ret;
    while ((ret = ogg_opus_next(&reader, &packet, &packet_len)) == 1) {
        assert(packet_len > 0 && packet_len <= OPUS_AUDIO_MAX_PACKET);
        packets++;
    }
    assert(ret == 0);
    assert(packets > 300);
    printf("%u packets, %uch, pre_skip=%u\n", packets, info.channels,
           info.pre_skip);

    assert(ogg_opus_open(&reader, data, (size_t)size - 1, &info) == 0);
    while ((ret = ogg_opus_next(&reader, &packet, &packet_len)) == 1)
        ;
    assert(ret == -1);

    data[0] = 'X';
    assert(ogg_opus_open(&reader, data, (size_t)size, &info) == -1);
    free(data);
    return 0;
}
