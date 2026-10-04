#ifndef SHA256_H
#define SHA256_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t state[8];
    uint64_t bytes;
    unsigned char block[64];
    size_t buffered;
} Sha256;

void sha256_init(Sha256 *hash);
void sha256_update(Sha256 *hash, const void *bytes, size_t length);
void sha256_finish(Sha256 *hash, unsigned char digest[32]);

#endif
