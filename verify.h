#ifndef VERIFY_H
#define VERIFY_H

#include "file_hash.h"

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t cluster_count;
    uint64_t cluster_bytes;
    const unsigned char *bitmap;
    unsigned char *owned;
    void (*read)(uint64_t offset, void *buffer, size_t length);
    uint64_t (*offset)(uint32_t cluster);
    uint32_t (*next)(uint32_t cluster);
    FileHashes *hashes;
} ExfatReader;

/* Checks supported directory sets and accounts for every allocated cluster.
 * Unknown in-use entry types are rejected rather than silently skipped. */
void exfat_verify(const ExfatReader *reader, uint32_t root);

#endif
