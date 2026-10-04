#include "patch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fail(const char *message) {
    fprintf(stderr, "error: %s\n", message);
    exit(1);
}

static size_t find(const PatchPlan *plan, uint64_t offset) {
    size_t low = 0, high = plan->count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        if (plan->items[middle].offset < offset) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return low;
}

void patch_read(void *context, uint64_t offset, void *buffer, size_t length) {
    PatchPlan *plan = context;
    if (offset > plan->length || length > plan->length - offset) {
        fail("patch read outside volume");
    }
    plan->read(plan->context, offset, buffer, length);
    uint64_t start = offset - offset % plan->sector;
    size_t index = find(plan, start);
    while (index < plan->count && plan->items[index].offset < offset + length) {
        const SectorPatch *item = &plan->items[index++];
        uint64_t begin = item->offset > offset ? item->offset : offset;
        uint64_t end = item->offset + plan->sector;
        if (end > offset + length) {
            end = offset + length;
        }
        memcpy((unsigned char *)buffer + (size_t)(begin - offset),
               item->after + (size_t)(begin - item->offset), (size_t)(end - begin));
    }
}

void patch_write(void *context, uint64_t offset, const void *buffer, size_t length) {
    PatchPlan *plan = context;
    if (offset > plan->length || length > plan->length - offset) {
        fail("patch write outside volume");
    }
    const unsigned char *source = buffer;
    while (length) {
        uint64_t sector_offset = offset - offset % plan->sector;
        size_t within = (size_t)(offset - sector_offset);
        size_t take = plan->sector - within;
        if (take > length) {
            take = length;
        }
        size_t index = find(plan, sector_offset);
        if (index == plan->count || plan->items[index].offset != sector_offset) {
            /* Bound memory/journal growth before any live writes. */
            if (plan->count >= (256u * 1024u * 1024u) / (2 * plan->sector + sizeof(SectorPatch))) {
                fail("metadata plan exceeds 256 MiB safety limit");
            }
            if (plan->count == plan->capacity) {
                size_t capacity = plan->capacity ? plan->capacity * 2 : 64;
                SectorPatch *grown = realloc(plan->items, capacity * sizeof(*grown));
                if (!grown) {
                    fail("out of memory building metadata plan");
                }
                plan->items = grown;
                plan->capacity = capacity;
            }
            unsigned char *bytes = malloc(plan->sector * 2);
            if (!bytes) {
                fail("out of memory for metadata sector");
            }
            plan->read(plan->context, sector_offset, bytes, plan->sector);
            memcpy(bytes + plan->sector, bytes, plan->sector);
            memmove(plan->items + index + 1, plan->items + index,
                    (plan->count - index) * sizeof(*plan->items));
            plan->items[index] = (SectorPatch){sector_offset, bytes, bytes + plan->sector};
            plan->count++;
        }
        memcpy(plan->items[index].after + within, source, take);
        offset += take;
        source += take;
        length -= take;
    }
}

void patch_free(PatchPlan *plan) {
    for (size_t i = 0; i < plan->count; i++) {
        free(plan->items[i].before);
    }
    free(plan->items);
    plan->items = NULL;
    plan->count = plan->capacity = 0;
}
