#define _CRT_SECURE_NO_WARNINGS
#include "patch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    unsigned char *bytes;
    size_t length;
} MemoryImage;
static void memory_read(void *context, uint64_t offset, void *buffer, size_t length) {
    MemoryImage *image = context;
    if (offset > image->length || length > image->length - offset) {
        exit(2);
    }
    memcpy(buffer, image->bytes + offset, length);
}
static MemoryImage load(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file || fseek(file, 0, SEEK_END)) {
        exit(2);
    }
    long length = ftell(file);
    if (length <= 0 || fseek(file, 0, SEEK_SET)) {
        exit(2);
    }
    unsigned char *bytes = malloc((size_t)length);
    if (!bytes || fread(bytes, 1, (size_t)length, file) != (size_t)length) {
        exit(2);
    }
    fclose(file);
    return (MemoryImage){bytes, (size_t)length};
}
int main(int argc, char **argv) {
    if (argc != 3) {
        return 2;
    }
    MemoryImage source = load(argv[1]), expected = load(argv[2]);
    unsigned char *baseline = malloc(source.length);
    if (!baseline) {
        return 2;
    }
    memcpy(baseline, source.bytes, source.length);
    ExfatLayout layout;
    FileHashes hashes = {0};
    if (exfat_validate_reader(memory_read, &source, source.length, 1, expected.length, &layout,
                              &hashes)) {
        return 1;
    }
    PatchPlan plan = {memory_read, &source, source.length, (size_t)layout.sector_bytes, NULL, 0, 0};
    exfat_plan_metadata(&layout, expected.length, patch_read, patch_write, &plan);
    unsigned char *virtual = malloc(expected.length);
    if (!virtual) {
        return 2;
    }
    patch_read(&plan, 0, virtual, expected.length);
    if (memcmp(virtual, expected.bytes, expected.length) ||
        memcmp(source.bytes, baseline, source.length)) {
        return 1;
    }
    for (size_t i = 0; i < plan.count; i++) {
        if (plan.items[i].offset + plan.sector > expected.length) {
            return 1;
        }
        memcpy(source.bytes + plan.items[i].offset, plan.items[i].after, plan.sector);
    }
    file_hash_begin_compare(&hashes);
    if (exfat_validate_reader(memory_read, &source, expected.length, 0, 0, NULL, &hashes) ||
        file_hash_report(&hashes)) {
        return 1;
    }
    if (memcmp(source.bytes, expected.bytes, expected.length) ||
        memcmp(source.bytes + expected.length, baseline + expected.length,
               source.length - expected.length)) {
        return 1;
    }
    for (size_t i = 0; i < plan.count; i++) {
        memcpy(source.bytes + plan.items[i].offset, plan.items[i].before, plan.sector);
    }
    if (memcmp(source.bytes, baseline, source.length)) {
        return 1;
    }
    printf("Patch apply, overlay, file hashes, unchanged tail, and byte-exact rollback passed (%zu "
           "sectors).\n",
           plan.count);
    patch_free(&plan);
    file_hash_free(&hashes);
    free(virtual);
    free(baseline);
    free(source.bytes);
    free(expected.bytes);
    return 0;
}
