#ifndef FILE_HASH_H
#define FILE_HASH_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    unsigned char identity[32];
    unsigned char digest[32];
    uint64_t length;
    int seen;
} FileHash;

typedef struct {
    FileHash *files;
    size_t count;
    size_t capacity;
    uint64_t source_bytes;
    uint64_t matched_bytes;
    size_t matched;
    size_t changed;
    size_t extra;
    int comparing;
    int listing;
} FileHashes;

void file_hash_record(FileHashes *hashes, const unsigned char identity[32],
                      const unsigned char digest[32], uint64_t length);
void file_hash_begin_compare(FileHashes *hashes);
int file_hash_report(const FileHashes *hashes);
void file_hash_free(FileHashes *hashes);

#endif
