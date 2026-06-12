/* EAN-13 scanline decoder (pure C, no IDF deps — host-testable).
 *
 * Feed one grayscale scanline; the decoder thresholds at the global
 * min/max midpoint, run-length encodes, and slides over candidate
 * start guards in both directions. A hit must pass the L/G/R width
 * tables, the parity-derived first digit and the checksum, so random
 * texture practically never false-positives.
 */
#ifndef EAN13_H
#define EAN13_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Returns 1 and writes "9784101010014\0" style digits into out when a
 * valid EAN-13 is found in the line (either direction), else 0. */
int ean13_decode_gray_line(const uint8_t *line, int n, char out[14]);

/* Extended scan with where/how-close telemetry for the viewfinder
 * overlay. digits = matched digits of the best candidate: 13 = valid
 * code (found=1, code filled), 12 = all digits read but checksum or
 * parity-table failed (the juiciest near-miss), lower = died earlier.
 * x0/x1 = pixel span of that candidate on the line (0/0 = none). */
typedef struct {
    int found;
    char code[14];
    int x0, x1;
    int digits;
} ean13_scan_t;

int ean13_scan_gray_line(const uint8_t *line, int n, ean13_scan_t *st);

#ifdef __cplusplus
}
#endif

#endif /* EAN13_H */
