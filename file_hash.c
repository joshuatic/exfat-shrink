#include "file_hash.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fail(const char *message) {
    fprintf(stderr, "error: %s\n", message);
    exit(1);
}

static int compare_identity(const void *left, const void *right) {
    const FileHash *a = left;
    const FileHash *b = right;

    return memcmp(a->identity, b->identity, 32);
}

static void print_digest(const unsigned char digest[32]) {
    for (unsigned i = 0; i < 32; i++) {
        printf("%02x", digest[i]);
    }
}

void file_hash_record(FileHashes *hashes, const unsigned char identity[32],
                      const unsigned char digest[32], uint64_t length) {
    if (hashes->listing) {
        printf("SHA256 ");
        print_digest(digest);
        printf(" BYTES %" PRIu64 " ID ", length);
        print_digest(identity);
        putchar('\n');
    }

    if (hashes->comparing) {
        FileHash key = {0};
        memcpy(key.identity, identity, 32);
        FileHash *expected = hashes->count ? bsearch(&key, hashes->files, hashes->count,
                                                     sizeof(*expected), compare_identity)
                                           : NULL;

        if (!expected) {
            hashes->extra++;
            return;
        }

        if (expected->seen) {
            fail("duplicate hashed file identity");
        }

        expected->seen = 1;

        if (expected->length == length && !memcmp(expected->digest, digest, 32)) {
            hashes->matched++;
            hashes->matched_bytes += length;
        } else {
            hashes->changed++;
            printf("File content/length mismatch, ID ");
            print_digest(identity);
            printf("\nExpected SHA256 ");
            print_digest(expected->digest);
            printf("\nActual SHA256   ");
            print_digest(digest);
            putchar('\n');
        }

        return;
    }

    if (hashes->source_bytes > UINT64_MAX - length) {
        fail("total file length overflow");
    }

    if (hashes->count == hashes->capacity) {
        size_t capacity = hashes->capacity ? hashes->capacity * 2 : 32;

        if (capacity < hashes->capacity || capacity > SIZE_MAX / sizeof(*hashes->files)) {
            fail("file hash manifest too large");
        }

        FileHash *grown = realloc(hashes->files, capacity * sizeof(*grown));

        if (!grown) {
            fail("out of memory for file hashes");
        }

        hashes->files = grown;
        hashes->capacity = capacity;
    }

    FileHash *entry = &hashes->files[hashes->count++];
    memcpy(entry->identity, identity, 32);
    memcpy(entry->digest, digest, 32);
    entry->length = length;
    entry->seen = 0;
    hashes->source_bytes += length;
}

void file_hash_begin_compare(FileHashes *hashes) {
    if (hashes->count > 1) {
        qsort(hashes->files, hashes->count, sizeof(*hashes->files), compare_identity);
    }

    for (size_t i = 0; i < hashes->count; i++) {
        hashes->files[i].seen = 0;

        if (i && !compare_identity(&hashes->files[i - 1], &hashes->files[i])) {
            fail("duplicate source file identity");
        }
    }

    hashes->comparing = 1;
    hashes->matched = 0;
    hashes->matched_bytes = 0;
    hashes->changed = 0;
    hashes->extra = 0;
}

int file_hash_report(const FileHashes *hashes) {
    size_t missing = 0;

    for (size_t i = 0; i < hashes->count; i++) {
        if (!hashes->files[i].seen) {
            missing++;
        }
    }

    printf("SHA-256 matched files: %zu / %zu\n", hashes->matched, hashes->count);
    printf("SHA-256 verified file bytes: %" PRIu64 " / %" PRIu64 "\n", hashes->matched_bytes,
           hashes->source_bytes);
    printf("Changed files: %zu; missing files: %zu; unexpected files: %zu\n", hashes->changed,
           missing, hashes->extra);

    if (hashes->changed || missing || hashes->extra) {
        fprintf(stderr, "error: file preservation check failed; output must not be used\n");
        return 1;
    }

    puts("All file contents preserved (SHA-256).");

    return 0;
}

void file_hash_free(FileHashes *hashes) {
    free(hashes->files);
    memset(hashes, 0, sizeof(*hashes));
}
