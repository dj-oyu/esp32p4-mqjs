/* PC round-trip test: reads pubkey(32) + signed-message from files,
 * verifies with crypto_sign_open, prints the recovered message. */
#include <stdio.h>
#include <stdlib.h>
#include "tweetnacl.h"

static long readfile(const char *p, unsigned char **out)
{
    FILE *f = fopen(p, "rb");
    if (!f) { perror(p); exit(2); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc(n);
    if (fread(b, 1, n, f) != (size_t)n) { exit(2); }
    fclose(f);
    *out = b;
    return n;
}

int main(int argc, char **argv)
{
    unsigned char *pk, *sm;
    long pkn = readfile(argv[1], &pk);
    long smn = readfile(argv[2], &sm);
    if (pkn != 32) { fprintf(stderr, "pubkey must be 32 bytes\n"); return 2; }

    unsigned char *m = malloc(smn + 1);
    unsigned long long mlen;
    int rc = crypto_sign_open(m, &mlen, sm, smn, pk);
    if (rc != 0) {
        printf("VERIFY FAIL\n");
        return 1;
    }
    m[mlen] = 0;
    printf("VERIFY OK (%llu bytes): %s\n", mlen, m);
    return 0;
}
