#ifndef EXFAT_H
#define EXFAT_H

#include "file_hash.h"

#include <stddef.h>
#include <stdint.h>
typedef void (*ExfatRead)(void *context, uint64_t offset, void *buffer, size_t length);
typedef void (*ExfatWrite)(void *context, uint64_t offset, const void *buffer, size_t length);

typedef struct {
    uint64_t sector_bytes;
    uint64_t cluster_bytes;
    uint64_t volume_bytes;
    uint64_t bitmap_entry_offset;
    uint64_t allocated_clusters;
    uint32_t fat_offset;
    uint32_t heap_offset;
    uint32_t cluster_count;
    uint32_t bitmap_first;
} ExfatLayout;

int exfat_validate_reader(ExfatRead read, void *context, uint64_t length, int plan, uint64_t target,
                          ExfatLayout *layout, FileHashes *hashes);
void exfat_plan_metadata(const ExfatLayout *layout, uint64_t target, ExfatRead read,
                         ExfatWrite write, void *context);

/* Read-only, single-image CLI analyzer. Exits on malformed or unsupported input.
 * This is not a reentrant library API and never authorizes truncating an image.
 * target_bytes is used only when plan is nonzero. Returns 0 on success. */
int exfat_analyze(const char *path, int plan, uint64_t target_bytes);

/* Returns layout only after supported consistency checks and target validation. */
int exfat_validate(const char *path, int plan, uint64_t target_bytes, ExfatLayout *layout);

/* Validates and hashes logical contents of every regular file in the image. */
int exfat_validate_hashes(const char *path, int plan, uint64_t target_bytes, ExfatLayout *layout,
                          FileHashes *hashes);
int exfat_compare_files(const char *source, const char *other);

/* Creates a new image with an empty tail removed. Source is never written. */
int exfat_shrink_copy(const char *source, const char *destination, uint64_t target_bytes);

#endif
