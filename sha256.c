#include "sha256.h"

#include <string.h>

static uint32_t rotate(uint32_t value, unsigned bits) {
    return value >> bits | value << (32 - bits);
}

static void transform(Sha256 *hash, const unsigned char block[64]) {
    static const uint32_t constants[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
        0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
        0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
        0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
        0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
        0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
        0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
        0xc67178f2};
    uint32_t words[64];

    for (unsigned i = 0; i < 16; i++) {
        const unsigned char *p = block + i * 4;
        words[i] = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
    }

    for (unsigned i = 16; i < 64; i++) {
        uint32_t a = words[i - 15];
        uint32_t b = words[i - 2];
        uint32_t s0 = rotate(a, 7) ^ rotate(a, 18) ^ (a >> 3);
        uint32_t s1 = rotate(b, 17) ^ rotate(b, 19) ^ (b >> 10);
        words[i] = words[i - 16] + s0 + words[i - 7] + s1;
    }

    uint32_t a = hash->state[0];
    uint32_t b = hash->state[1];
    uint32_t c = hash->state[2];
    uint32_t d = hash->state[3];
    uint32_t e = hash->state[4];
    uint32_t f = hash->state[5];
    uint32_t g = hash->state[6];
    uint32_t h = hash->state[7];

    for (unsigned i = 0; i < 64; i++) {
        uint32_t s1 = rotate(e, 6) ^ rotate(e, 11) ^ rotate(e, 25);
        uint32_t choice = (e & f) ^ (~e & g);
        uint32_t t1 = h + s1 + choice + constants[i] + words[i];
        uint32_t s0 = rotate(a, 2) ^ rotate(a, 13) ^ rotate(a, 22);
        uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    hash->state[0] += a;
    hash->state[1] += b;
    hash->state[2] += c;
    hash->state[3] += d;
    hash->state[4] += e;
    hash->state[5] += f;
    hash->state[6] += g;
    hash->state[7] += h;
}

void sha256_init(Sha256 *hash) {
    static const uint32_t initial[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(hash->state, initial, sizeof(initial));
    hash->bytes = 0;
    hash->buffered = 0;
}

void sha256_update(Sha256 *hash, const void *bytes, size_t length) {
    const unsigned char *input = bytes;
    hash->bytes += length;

    while (length) {
        if (!hash->buffered && length >= 64) {
            transform(hash, input);
            input += 64;
            length -= 64;
            continue;
        }

        size_t take = 64 - hash->buffered;

        if (take > length) {
            take = length;
        }

        memcpy(hash->block + hash->buffered, input, take);
        hash->buffered += take;
        input += take;
        length -= take;

        if (hash->buffered == 64) {
            transform(hash, hash->block);
            hash->buffered = 0;
        }
    }
}

void sha256_finish(Sha256 *hash, unsigned char digest[32]) {
    uint64_t bits = hash->bytes * 8;
    unsigned char padding[128] = {0x80};
    size_t length = hash->buffered < 56 ? 56 - hash->buffered : 120 - hash->buffered;

    for (unsigned i = 0; i < 8; i++) {
        padding[length + i] = (unsigned char)(bits >> (56 - i * 8));
    }

    sha256_update(hash, padding, length + 8);

    for (unsigned i = 0; i < 8; i++) {
        for (unsigned j = 0; j < 4; j++) {
            digest[i * 4 + j] = (unsigned char)(hash->state[i] >> (24 - j * 8));
        }
    }
}
