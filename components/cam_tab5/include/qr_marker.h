#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    int markers;
    bool inverted; /* finder modules are brighter than their background */
} qr_marker_result_t;

/* Lightweight QR finder-pattern prefilter. Detects three clustered
 * 1:1:3:1:1 finder centers forming a non-degenerate triangle. */
bool qr_marker_detect(const uint8_t *gray, int w, int h,
                      qr_marker_result_t *result);
