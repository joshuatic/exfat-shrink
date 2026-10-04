#include "exfat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void fail(const char *message) {
    fprintf(stderr, "error: %s\n", message);
    exit(1);
}
static uint32_t u32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void put32(unsigned char *p, uint32_t value) {
    for (unsigned i = 0; i < 4; i++) {
        p[i] = (unsigned char)(value >> (i * 8));
    }
}

static void put64(unsigned char *p, uint64_t value) {
    put32(p, (uint32_t)value);
    put32(p + 4, (uint32_t)(value >> 32));
}

static uint64_t cluster_offset(const ExfatLayout *layout, uint32_t cluster) {
    if (cluster < 2 || (uint64_t)cluster >= (uint64_t)layout->cluster_count + 2) {
        fail("invalid bitmap cluster");
    }

    return (uint64_t)layout->heap_offset * layout->sector_bytes +
           (uint64_t)(cluster - 2) * layout->cluster_bytes;
}

static void update_boot(unsigned char *boot, const ExfatLayout *layout, uint64_t target,
                        uint32_t clusters, uint64_t allocated) {
    put64(boot + 72, target / layout->sector_bytes);
    put32(boot + 92, clusters);
    boot[112] = (unsigned char)(allocated * 100 / clusters);

    uint32_t checksum = 0;

    for (size_t i = 0; i < 11 * layout->sector_bytes; i++) {
        if (i != 106 && i != 107 && i != 112) {
            checksum = (checksum >> 1 | checksum << 31) + boot[i];
        }
    }

    for (size_t i = 11 * layout->sector_bytes; i < 12 * layout->sector_bytes; i += 4) {
        put32(boot + i, checksum);
    }
}

void exfat_plan_metadata(const ExfatLayout *layout, uint64_t target, ExfatRead read,
                         ExfatWrite write, void *context) {
    unsigned char *buffer = malloc(1024 * 1024);
    if (!buffer) {
        fail("out of memory");
    }
    uint64_t new_count64 =
        (target - (uint64_t)layout->heap_offset * layout->sector_bytes) / layout->cluster_bytes;

    if (!new_count64 || new_count64 >= layout->cluster_count) {
        fail("target must remove at least one heap cluster");
    }

    uint32_t new_count = (uint32_t)new_count64;
    size_t old_length = (size_t)(((uint64_t)layout->cluster_count + 7) / 8);
    size_t new_length = (size_t)((new_count64 + 7) / 8);
    size_t old_chain_length =
        (size_t)((old_length + layout->cluster_bytes - 1) / layout->cluster_bytes);
    size_t new_chain_length =
        (size_t)((new_length + layout->cluster_bytes - 1) / layout->cluster_bytes);
    unsigned char *bitmap = malloc(old_length);
    uint32_t *chain = malloc(old_chain_length * sizeof(*chain));

    if (!bitmap || !chain) {
        fail("out of memory");
    }

    uint32_t cluster = layout->bitmap_first;
    size_t loaded = 0;

    for (size_t i = 0; i < old_chain_length; i++) {
        chain[i] = cluster;
        size_t length = old_length - loaded;

        if (length > layout->cluster_bytes) {
            length = (size_t)layout->cluster_bytes;
        }

        read(context, cluster_offset(layout, cluster), bitmap + loaded, length);
        loaded += length;

        unsigned char fat_entry[4];
        read(context, (uint64_t)layout->fat_offset * layout->sector_bytes + (uint64_t)cluster * 4,
             fat_entry, sizeof(fat_entry));
        cluster = u32(fat_entry);
    }

    if (cluster != UINT32_MAX) {
        fail("bitmap changed after validation");
    }

    /* Release bitmap clusters made unnecessary by the smaller heap. */
    for (size_t i = new_chain_length; i < old_chain_length; i++) {
        uint32_t index = chain[i] - 2;
        bitmap[index / 8] &= (unsigned char)~(1u << (index % 8));
    }

    if (new_count % 8) {
        bitmap[new_length - 1] &= (unsigned char)((1u << (new_count % 8)) - 1);
    }

    memset(bitmap + new_length, 0, old_length - new_length);

    loaded = 0;

    for (size_t i = 0; i < new_chain_length; i++) {
        size_t length = old_length - loaded;

        if (length > layout->cluster_bytes) {
            length = (size_t)layout->cluster_bytes;
        }

        write(context, cluster_offset(layout, chain[i]), bitmap + loaded, length);
        loaded += length;
    }

    unsigned char value[8];
    put32(value, UINT32_MAX);
    uint64_t fat_start = (uint64_t)layout->fat_offset * layout->sector_bytes;
    write(context, fat_start + (uint64_t)chain[new_chain_length - 1] * 4, value, 4);
    put32(value, 0);

    for (size_t i = new_chain_length; i < old_chain_length; i++) {
        write(context, fat_start + (uint64_t)chain[i] * 4, value, 4);
    }

    /* Removed cluster indices must no longer carry stale FAT references. */
    memset(buffer, 0, 1024 * 1024);
    uint64_t fat_position = fat_start + (new_count64 + 2) * 4;
    uint64_t fat_end = fat_start + ((uint64_t)layout->cluster_count + 2) * 4;

    while (fat_position < fat_end) {
        size_t length =
            (size_t)(fat_end - fat_position < 1024 * 1024 ? fat_end - fat_position : 1024 * 1024);
        write(context, fat_position, buffer, length);
        fat_position += length;
    }

    put64(value, new_length);
    write(context, layout->bitmap_entry_offset + 24, value, sizeof(value));

    size_t boot_length = (size_t)(24 * layout->sector_bytes);
    read(context, 0, buffer, boot_length);
    uint64_t allocated = layout->allocated_clusters - (old_chain_length - new_chain_length);
    update_boot(buffer, layout, target, new_count, allocated);
    update_boot(buffer + 12 * layout->sector_bytes, layout, target, new_count, allocated);
    write(context, 0, buffer, boot_length);

    free(buffer);
    free(chain);
    free(bitmap);
}
