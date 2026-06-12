/* EAN-13 scanline decoder. See include/ean13.h.
 *
 * Geometry: 95 modules = start guard (bar,space,bar) + 6 left digits
 * (4 runs each, starting with a space) + middle guard (5 runs) +
 * 6 right digits (4 runs each, starting with a bar) + end guard.
 * Left digits encode in L (odd) or G (even) parity; the 6-digit parity
 * pattern encodes the 13th (leading) digit. Right digits use R codes,
 * whose run-width sequence equals the L table read from a bar.
 */
#include "ean13.h"

#include <string.h>

#define MAX_RUNS 1024

/* run widths in modules for L-codes, runs read space-first.
 * G(d) = the same row reversed; R(d) = same row read bar-first. */
static const uint8_t L_RUNS[10][4] = {
    { 3, 2, 1, 1 }, { 2, 2, 2, 1 }, { 2, 1, 2, 2 }, { 1, 4, 1, 1 },
    { 1, 1, 3, 2 }, { 1, 2, 3, 1 }, { 1, 1, 1, 4 }, { 1, 3, 1, 2 },
    { 1, 2, 1, 3 }, { 3, 1, 1, 2 }
};

static const char *const PARITY[10] = {
    "LLLLLL", "LLGLGG", "LLGGLG", "LLGGGL", "LGLLGG",
    "LGGLLG", "LGGGLL", "LGLGLG", "LGLGGL", "LGGLGL"
};

/* error of 4 runs against a pattern, in 1/64 module units; large when
 * any single run is more than one module off */
static int digit_err(const int w[4], int total, const uint8_t pat[4])
{
    int err = 0;
    for (int j = 0; j < 4; j++) {
        int m = (w[j] * 7 * 64 + total / 2) / total;
        int d = m - pat[j] * 64;
        if (d < 0)
            d = -d;
        if (d > 64)
            return 1 << 20;
        err += d;
    }
    return err;
}

/* best digit for 4 runs. left=1: try L and reversed (=G) rows and set
 * *par; left=0: right digit (R widths == L widths bar-first). */
static int match_digit(const int w[4], int left, char *par)
{
    int total = w[0] + w[1] + w[2] + w[3];
    if (total <= 0)
        return -1;
    int best = -1;
    int best_err = 96; /* cumulative cap: 1.5 modules */
    char best_par = 'L';
    for (int d = 0; d < 10; d++) {
        int e = digit_err(w, total, L_RUNS[d]);
        if (e < best_err) {
            best_err = e;
            best = d;
            best_par = 'L';
        }
        if (left) {
            uint8_t rev[4] = { L_RUNS[d][3], L_RUNS[d][2],
                               L_RUNS[d][1], L_RUNS[d][0] };
            e = digit_err(w, total, rev);
            if (e < best_err) {
                best_err = e;
                best = d;
                best_par = 'G';
            }
        }
    }
    if (par)
        *par = best_par;
    return best;
}

/* all runs of a guard within [0.5, 1.8] modules? */
static int guard_ok(const int *runs, int n, int total)
{
    for (int k = 0; k < n; k++) {
        int r = runs[k] * n * 64 / total; /* width in 1/64 of a module */
        if (r < 32 || r > 115)
            return 0;
    }
    return 1;
}

static int try_decode_at(const int *runs, int nruns, int i, char out[14])
{
    if (i + 59 > nruns)
        return 0;

    int g = runs[i] + runs[i + 1] + runs[i + 2];
    if (g < 5) /* under ~1.7px per module: optically hopeless (checksum
                  + parity + guards keep false positives out even at
                  this gate — see the noise test in tools/) */
        return 0;
    if (!guard_ok(runs + i, 3, g))
        return 0;
    int module = g / 3;
    /* quiet zone before the start guard (lenient: >= 3 modules) */
    if (i > 0 && runs[i - 1] < module * 3)
        return 0;

    int digits[13];
    char par[7] = { 0 };
    int pos = i + 3;

    for (int d = 0; d < 6; d++, pos += 4) {
        int w[4] = { runs[pos], runs[pos + 1], runs[pos + 2], runs[pos + 3] };
        int dig = match_digit(w, 1, &par[d]);
        if (dig < 0)
            return 0;
        digits[d + 1] = dig;
    }

    int mg = runs[pos] + runs[pos + 1] + runs[pos + 2] + runs[pos + 3] +
             runs[pos + 4];
    if (!guard_ok(runs + pos, 5, mg))
        return 0;
    pos += 5;

    for (int d = 0; d < 6; d++, pos += 4) {
        int w[4] = { runs[pos], runs[pos + 1], runs[pos + 2], runs[pos + 3] };
        int dig = match_digit(w, 0, 0);
        if (dig < 0)
            return 0;
        digits[d + 7] = dig;
    }

    int eg = runs[pos] + runs[pos + 1] + runs[pos + 2];
    if (!guard_ok(runs + pos, 3, eg))
        return 0;
    /* quiet zone after (or the line simply ends there) */
    if (pos + 3 < nruns && runs[pos + 3] < module * 3)
        return 0;

    digits[0] = -1;
    for (int d = 0; d < 10; d++) {
        if (memcmp(par, PARITY[d], 6) == 0) {
            digits[0] = d;
            break;
        }
    }
    if (digits[0] < 0)
        return 0;

    int sum = 0;
    for (int k = 0; k < 12; k++)
        sum += digits[k] * ((k & 1) ? 3 : 1);
    if ((10 - sum % 10) % 10 != digits[12])
        return 0;

    for (int k = 0; k < 13; k++)
        out[k] = (char)('0' + digits[k]);
    out[13] = '\0';
    return 1;
}

/* walk every bar run as a candidate start guard */
static int scan_runs(const int *runs, int nruns, int first_black,
                     char out[14])
{
    for (int i = 0; i + 59 <= nruns; i++) {
        int is_bar = first_black ? !(i & 1) : (i & 1);
        if (!is_bar)
            continue;
        if (try_decode_at(runs, nruns, i, out))
            return 1;
    }
    return 0;
}

int ean13_decode_gray_line(const uint8_t *line, int n, char out[14])
{
    if (n < 120)
        return 0;

    uint8_t mn = 255, mx = 0;
    for (int i = 0; i < n; i++) {
        if (line[i] < mn)
            mn = line[i];
        if (line[i] > mx)
            mx = line[i];
    }
    if (mx - mn < 48) /* flat line: no barcode contrast */
        return 0;
    int th = (mn + mx) / 2;

    int runs[MAX_RUNS];
    int nruns = 0;
    int cur = line[0] < th;
    int len = 1;
    int first_black = cur;
    for (int i = 1; i < n; i++) {
        int b = line[i] < th;
        if (b == cur) {
            len++;
            continue;
        }
        if (nruns >= MAX_RUNS)
            return 0;
        runs[nruns++] = len;
        cur = b;
        len = 1;
    }
    if (nruns >= MAX_RUNS)
        return 0;
    runs[nruns++] = len;

    if (scan_runs(runs, nruns, first_black, out))
        return 1;

    /* reversed direction (camera upside down / right-to-left sweep) */
    for (int i = 0, j = nruns - 1; i < j; i++, j--) {
        int t = runs[i];
        runs[i] = runs[j];
        runs[j] = t;
    }
    int first_black_rev = first_black ^ ((nruns - 1) & 1);
    return scan_runs(runs, nruns, first_black_rev, out);
}
