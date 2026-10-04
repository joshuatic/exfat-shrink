#ifndef PATCH_H
#define PATCH_H
#include "exfat.h"
typedef struct {
    uint64_t offset;
    unsigned char *before;
    unsigned char *after;
} SectorPatch;
typedef struct {
    ExfatRead read;
    void *context;
    uint64_t length;
    size_t sector;
    SectorPatch *items;
    size_t count;
    size_t capacity;
} PatchPlan;
void patch_read(void *context, uint64_t offset, void *buffer, size_t length);
void patch_write(void *context, uint64_t offset, const void *buffer, size_t length);
void patch_free(PatchPlan *plan);
#endif
