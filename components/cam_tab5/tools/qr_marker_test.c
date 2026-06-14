/* gcc -O2 -I../include -o /tmp/qr_marker_test qr_marker_test.c ../qr_marker.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/qr_marker.h"

#define W 400
#define H 300

static uint8_t image[W * H];
static int fails;

static void expect(int ok, const char *name)
{
    printf("%s %s\n", ok ? "ok" : "FAIL", name);
    if (!ok)
        fails++;
}

static void rect(int x0, int y0, int x1, int y1, uint8_t value)
{
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
            image[y * W + x] = value;
}

static void finder(int cx, int cy, int module, int inverted)
{
    uint8_t fg = inverted ? 240 : 15;
    uint8_t bg = inverted ? 15 : 240;
    int half = 7 * module / 2;
    rect(cx - half, cy - half, cx + half + 1, cy + half + 1, fg);
    rect(cx - half + module, cy - half + module,
         cx + half + 1 - module, cy + half + 1 - module, bg);
    rect(cx - half + 2 * module, cy - half + 2 * module,
         cx + half + 1 - 2 * module, cy + half + 1 - 2 * module, fg);
}

static void case_markers(int inverted, int count, const char *name)
{
    memset(image, inverted ? 15 : 240, sizeof image);
    finder(80, 70, 6, inverted);
    finder(300, 70, 6, inverted);
    if (count == 3)
        finder(80, 235, 6, inverted);
    qr_marker_result_t r = { 0 };
    int ok = qr_marker_detect(image, W, H, &r);
    expect((count == 3) == ok && (!ok || r.inverted == inverted), name);
}

int main(void)
{
    case_markers(0, 3, "normal three finders");
    case_markers(1, 3, "inverted three finders");
    case_markers(0, 2, "two finders rejected");
    for (int i = 0; i < W * H; i++)
        image[i] = (uint8_t)rand();
    qr_marker_result_t r = { 0 };
    expect(!qr_marker_detect(image, W, H, &r), "noise rejected");
    return fails ? 1 : 0;
}
