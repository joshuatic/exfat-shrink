#define _CRT_SECURE_NO_WARNINGS
#include "sha256.h"

#include <stdio.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

int main(void) {
#ifdef _WIN32
    if (_setmode(_fileno(stdin), _O_BINARY) == -1) {
        return 1;
    }
#endif

    /* Deliberately use an odd size to exercise buffered block boundaries. */
    unsigned char buffer[113];
    Sha256 hash;
    sha256_init(&hash);
    size_t length;

    while ((length = fread(buffer, 1, sizeof(buffer), stdin)) != 0) {
        sha256_update(&hash, buffer, length);
    }

    if (ferror(stdin)) {
        return 1;
    }

    unsigned char digest[32];
    sha256_finish(&hash, digest);

    for (unsigned i = 0; i < 32; i++) {
        printf("%02x", digest[i]);
    }

    putchar('\n');

    return 0;
}
