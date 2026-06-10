#ifndef TWEETNACL_H
#define TWEETNACL_H

/* Verify-only subset of TweetNaCl (D. J. Bernstein et al., public domain).
 * sm = signature(64) || message; on success m holds the message and
 * *mlen its length. Returns 0 if the signature is valid, -1 otherwise.
 * m must have room for n bytes. */
int crypto_sign_open(unsigned char *m, unsigned long long *mlen,
                     const unsigned char *sm, unsigned long long n,
                     const unsigned char *pk);

#endif
