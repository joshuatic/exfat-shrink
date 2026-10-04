#define _CRT_SECURE_NO_WARNINGS
#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L
#include "exfat.h"
#include "verify.h"

#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static FILE *image;
static ExfatRead external_read;
static void *external_context;
static uint64_t image_size;
static uint64_t sector;
static uint64_t spc;
static uint64_t cluster_bytes;
static uint64_t volume;
static uint32_t fat;
static uint32_t heap;
static uint32_t count;

static void fail(const char *s) {
    fprintf(stderr, "error: %s\n", s);
    exit(1);
}

static uint16_t u16(const unsigned char *p) {
    return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t u32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t u64(const unsigned char *p) {
    return u32(p) | (uint64_t)u32(p + 4) << 32;
}

static void read_at(uint64_t pos, void *buf, size_t n) {
    if (pos > image_size || n > image_size - pos || pos > INT64_MAX) {
        fail("read outside image");
    }
    if (external_read) {
        external_read(external_context, pos, buf, n);
        return;
    }
#ifdef _WIN32
    if (_fseeki64(image, (int64_t)pos, SEEK_SET)) {
        fail("seek failed");
    }
#else
    if (fseeko(image, (off_t)pos, SEEK_SET)) {
        fail("seek failed");
    }
#endif
    if (fread(buf, 1, n, image) != n) {
        fail("short read");
    }
}

static int valid_cluster(uint32_t c) {
    return c >= 2 && (uint64_t)c < (uint64_t)count + 2;
}

static uint64_t cluster_offset(uint32_t c) {
    if (!valid_cluster(c)) {
        fail("cluster outside heap");
    }

    return ((uint64_t)heap + (uint64_t)(c - 2) * spc) * sector;
}

static uint32_t next_cluster(uint32_t c) {
    unsigned char b[4];
    read_at((uint64_t)fat * sector + (uint64_t)c * 4, b, 4);

    uint32_t next = u32(b);
    if (next != UINT32_MAX && !valid_cluster(next)) {
        fail("invalid FAT chain");
    }

    return next;
}

static void boot_check(const unsigned char *b) {
    uint32_t sum = 0;
    if (memcmp(b,
               "\xeb\x76\x90"
               "EXFAT   ",
               11) ||
        u16(b + 510) != 0xaa55) {
        fail("invalid boot signature");
    }

    for (size_t i = 11; i < 64; i++) {
        if (b[i]) {
            fail("nonzero legacy BPB");
        }
    }

    for (size_t i = 1; i <= 8; i++) {
        if (u32(b + (i + 1) * sector - 4) != 0xaa550000) {
            fail("invalid extended boot signature");
        }
    }

    for (size_t i = 0; i < 11 * sector; i++) {
        if (i == 106 || i == 107 || i == 112) {
            continue;
        }

        sum = (sum >> 1 | sum << 31) + b[i];
    }

    for (size_t i = 11 * sector; i < 12 * sector; i += 4) {
        if (u32(b + i) != sum) {
            fail("boot checksum mismatch");
        }
    }
}

static int validate_reader(int plan, uint64_t target, ExfatLayout *layout, FileHashes *hashes) {
    unsigned char header[512];
    read_at(0, header, sizeof header);
    if (header[108] < 9 || header[108] > 12 || header[109] > 25 - header[108]) {
        fail("invalid sector or cluster shift");
    }

    sector = UINT64_C(1) << header[108];
    spc = UINT64_C(1) << header[109];
    cluster_bytes = sector * spc;

    unsigned char *boot = malloc((size_t)(24 * sector));
    if (!boot) {
        fail("out of memory");
    }

    read_at(0, boot, (size_t)(24 * sector));
    boot_check(boot);
    boot_check(boot + 12 * sector);
    for (size_t i = 0; i < 11 * sector; i++) {
        if (i != 106 && i != 107 && i != 112 && boot[i] != boot[12 * sector + i]) {
            fail("boot regions disagree");
        }
    }

    volume = u64(boot + 72);
    fat = u32(boot + 80);

    uint32_t fat_length = u32(boot + 84);
    heap = u32(boot + 88);
    count = u32(boot + 92);

    uint32_t root = u32(boot + 96);
    if (u16(boot + 104) != 0x100 || boot[110] != 1) {
        fail("only exFAT 1.00 with one FAT is supported");
    }

    if (u16(boot + 106) != 0) {
        fail("volume flags indicate dirty, failed, or unsupported state");
    }

    if (boot[112] > 100 && boot[112] != 255) {
        fail("invalid percent in use");
    }

    if (!count || count > 0xfffffff5u || !valid_cluster(root) || fat < 24 || !fat_length ||
        (uint64_t)fat + fat_length > heap ||
        (uint64_t)fat_length * sector < ((uint64_t)count + 2) * 4 || volume > image_size / sector ||
        volume < ((uint64_t)heap + (uint64_t)count * spc)) {
        fail("invalid volume geometry or truncated image");
    }

    unsigned char reserved_fat[8];
    read_at((uint64_t)fat * sector, reserved_fat, sizeof(reserved_fat));

    if ((u32(reserved_fat) & 0xffffff00u) != 0xffffff00u || u32(reserved_fat + 4) != UINT32_MAX) {
        fail("invalid reserved FAT entries");
    }

    free(boot);
    /* A visited bitset bounds walks and rejects cycles before repeated reads. */
    size_t bit_bytes = (size_t)(((uint64_t)count + 7) / 8);

    unsigned char *seen = calloc(bit_bytes, 1);
    unsigned char *bitmap = malloc(bit_bytes);
    if (!seen || !bitmap) {
        fail("out of memory");
    }

    uint64_t bitmap_entry_offset = 0;
    uint32_t bitmap_first = 0;

    uint64_t bitmap_length = 0;
    int ended = 0;
    for (uint32_t c = root; c != UINT32_MAX; c = next_cluster(c)) {
        size_t idx = (c - 2) / 8;
        unsigned mask = 1u << ((c - 2) % 8);
        if (seen[idx] & mask) {
            fail("root directory FAT cycle");
        }

        seen[idx] |= (unsigned char)mask;
        if (ended) {
            continue;
        }

        for (uint64_t p = 0; p < cluster_bytes; p += 32) {
            unsigned char entry[32];
            read_at(cluster_offset(c) + p, entry, 32);
            if (!entry[0]) {
                ended = 1;
                break;
            }

            if (entry[0] == 0x81) {
                if (bitmap_first || entry[1] != 0) {
                    fail("duplicate or unsupported allocation bitmap");
                }

                bitmap_entry_offset = cluster_offset(c) + p;
                bitmap_first = u32(entry + 20);
                bitmap_length = u64(entry + 24);
                if (!valid_cluster(bitmap_first)) {
                    fail("invalid bitmap first cluster");
                }
            }
        }
    }

    if (!bitmap_first || bitmap_length != bit_bytes) {
        fail("missing or invalid allocation bitmap length");
    }

    uint64_t loaded = 0;
    for (uint32_t c = bitmap_first; c != UINT32_MAX; c = next_cluster(c)) {
        size_t idx = (c - 2) / 8;
        unsigned mask = 1u << ((c - 2) % 8);
        if (seen[idx] & mask) {
            fail("bitmap cycle or overlap with root");
        }

        seen[idx] |= (unsigned char)mask;
        if (loaded == bitmap_length) {
            fail("bitmap chain longer than data length");
        }

        size_t n = (size_t)(bitmap_length - loaded < cluster_bytes ? bitmap_length - loaded
                                                                   : cluster_bytes);
        read_at(cluster_offset(c), bitmap + (size_t)loaded, n);
        loaded += n;
    }

    if (loaded != bitmap_length) {
        fail("bitmap chain too short");
    }

    uint64_t used = 0;
    uint64_t retained = 0;
    for (uint32_t i = 0; i < count; i++) {
        unsigned mask = 1u << (i % 8);
        int allocated = (bitmap[i / 8] & mask) != 0;
        if ((seen[i / 8] & mask) && !allocated) {
            fail("metadata cluster marked free");
        }

        if (allocated) {
            used++;
            retained = (uint64_t)i + 1;
        }
    }

    ExfatReader reader = {count,   cluster_bytes,  bitmap,       seen,
                          read_at, cluster_offset, next_cluster, hashes};
    exfat_verify(&reader, root);

    uint64_t minimum = ((uint64_t)heap + retained * spc) * sector;
    printf("Volume bytes: %" PRIu64 "\nCluster bytes: %" PRIu64 "\nClusters: %" PRIu32
           "\nAllocated clusters: %" PRIu64 "\nBitmap tail boundary bytes: %" PRIu64 "\n",
           volume * sector, cluster_bytes, count, used, minimum);
    puts("Read-only verification complete; this check made no changes.");

    int result = 0;
    if (plan) {
        if (target % sector || target >= volume * sector || target < minimum) {
            puts("Target rejected: must be sector aligned, smaller than the volume, and retain "
                 "every allocated cluster.");
            result = 1;
        } else {
            uint64_t new_count = (target / sector - heap) / spc;
            if (new_count > count) {
                new_count = count;
            }

            printf("Candidate retained clusters: %" PRIu64 "\nCandidate removed clusters: %" PRIu64
                   "\n",
                   new_count, (uint64_t)count - new_count);
            puts("Candidate only. No data or metadata written. No partition resize performed.");
        }
    }

    if (layout && !result) {
        *layout = (ExfatLayout){sector, cluster_bytes, volume * sector, bitmap_entry_offset, used,
                                fat,    heap,          count,           bitmap_first};
    }

    free(seen);
    free(bitmap);

    return result;
}

int exfat_validate_reader(ExfatRead read, void *context, uint64_t length, int plan, uint64_t target,
                          ExfatLayout *layout, FileHashes *hashes) {
    if (!read || length < 512) {
        fail("invalid reader or volume length");
    }
    external_read = read;
    external_context = context;
    image_size = length;
    int result = validate_reader(plan, target, layout, hashes);
    external_read = NULL;
    external_context = NULL;
    return result;
}

int exfat_validate_hashes(const char *path, int plan, uint64_t target, ExfatLayout *layout,
                          FileHashes *hashes) {
    image = fopen(path, "rb");
    if (!image) {
        fail("cannot open image");
    }
#ifdef _WIN32
    struct _stat64 st;
    if (_fstat64(_fileno(image), &st) || (st.st_mode & _S_IFMT) != _S_IFREG) {
        fail("only regular image files are supported");
    }
#else
    struct stat st;
    if (fstat(fileno(image), &st) || !S_ISREG(st.st_mode)) {
        fail("only regular image files are supported");
    }
#endif
    if (st.st_size < 512) {
        fail("image too short");
    }

    external_read = NULL;
    image_size = (uint64_t)st.st_size;
    int result = validate_reader(plan, target, layout, hashes);
    if (fclose(image)) {
        fail("close failed");
    }
    image = NULL;
    return result;
}

int exfat_analyze(const char *path, int plan, uint64_t target) {
    return exfat_validate(path, plan, target, NULL);
}

int exfat_validate(const char *path, int plan, uint64_t target, ExfatLayout *layout) {
    return exfat_validate_hashes(path, plan, target, layout, NULL);
}

int exfat_compare_files(const char *source, const char *other) {
    FileHashes hashes = {0};
    exfat_validate_hashes(source, 0, 0, NULL, &hashes);
    file_hash_begin_compare(&hashes);
    exfat_validate_hashes(other, 0, 0, NULL, &hashes);
    int result = file_hash_report(&hashes);
    file_hash_free(&hashes);

    return result;
}
