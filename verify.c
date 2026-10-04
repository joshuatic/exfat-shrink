#define _CRT_SECURE_NO_WARNINGS
#include "verify.h"
#include "sha256.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ENTRY_BYTES 32
#define MAX_SET_BYTES (256 * ENTRY_BYTES)
#define MAX_DIRECTORY_BYTES (UINT64_C(256) * 1024 * 1024)

typedef struct {
    uint32_t first;
    uint64_t length;
    int contiguous;
} Stream;

typedef struct {
    Stream stream;
    unsigned char identity[32];
} Directory;

typedef struct {
    const ExfatReader *reader;
    Stream stream;
    uint32_t cluster;
    uint64_t position;
    uint64_t cluster_position;
} Cursor;

typedef struct {
    uint16_t length;
    uint16_t text[255];
} Name;

static void fail(const char *message) {
    fprintf(stderr, "error: %s\n", message);
    exit(1);
}

static uint16_t u16(const unsigned char *p) {
    return (uint16_t)(p[0] | (uint16_t)p[1] << 8);
}

static uint32_t u32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t u64(const unsigned char *p) {
    return u32(p) | (uint64_t)u32(p + 4) << 32;
}

static uint16_t rotate16(uint16_t value, unsigned char byte) {
    return (uint16_t)((value >> 1 | value << 15) + byte);
}

static int bit(const unsigned char *bits, uint32_t index) {
    return (bits[index / 8] & (1u << (index % 8))) != 0;
}

static void claim(const ExfatReader *r, uint32_t cluster) {
    if (cluster < 2 || (uint64_t)cluster >= (uint64_t)r->cluster_count + 2) {
        fail("stream cluster outside heap");
    }

    uint32_t index = cluster - 2;

    if (!bit(r->bitmap, index)) {
        fail("stream cluster marked free");
    }

    if (bit(r->owned, index)) {
        fail("cross-linked allocation or FAT cycle");
    }

    r->owned[index / 8] |= (unsigned char)(1u << (index % 8));
}

static void claim_stream(const ExfatReader *r, Stream stream) {
    uint64_t clusters = stream.length / r->cluster_bytes;

    if (stream.length % r->cluster_bytes) {
        clusters++;
    }

    if (!clusters) {
        if (stream.first || stream.contiguous) {
            fail("invalid empty stream");
        }

        return;
    }

    if (clusters > r->cluster_count) {
        fail("stream length exceeds heap");
    }

    uint32_t cluster = stream.first;

    for (uint64_t i = 0; i < clusters; i++) {
        claim(r, cluster);

        if (stream.contiguous) {
            cluster++;
        } else {
            cluster = r->next(cluster);

            if (cluster == UINT32_MAX && i + 1 != clusters) {
                fail("stream FAT chain shorter than data length");
            }
        }
    }

    if (!stream.contiguous && cluster != UINT32_MAX) {
        fail("stream FAT chain longer than data length");
    }
}

static Cursor cursor(const ExfatReader *r, Stream stream) {
    Cursor result = {r, stream, stream.first, 0, 0};

    return result;
}

/* Only called after the stream's allocation has been bounded and verified. */
static int read_entry(Cursor *c, unsigned char entry[ENTRY_BYTES]) {
    if (c->position == c->stream.length) {
        return 0;
    }

    if (c->stream.length - c->position < ENTRY_BYTES) {
        fail("partial directory entry");
    }

    if (c->cluster_position == c->reader->cluster_bytes) {
        c->cluster = c->stream.contiguous ? c->cluster + 1 : c->reader->next(c->cluster);
        c->cluster_position = 0;
    }

    c->reader->read(c->reader->offset(c->cluster) + c->cluster_position, entry, ENTRY_BYTES);
    c->position += ENTRY_BYTES;
    c->cluster_position += ENTRY_BYTES;

    return 1;
}

static unsigned char *read_stream(const ExfatReader *r, Stream stream) {
    unsigned char *bytes = malloc((size_t)stream.length);

    if (!bytes) {
        fail("out of memory");
    }

    uint64_t position = 0;
    uint32_t cluster = stream.first;

    while (position < stream.length) {
        size_t length = (size_t)(stream.length - position);

        if (length > r->cluster_bytes) {
            length = (size_t)r->cluster_bytes;
        }

        r->read(r->offset(cluster), bytes + (size_t)position, length);
        position += length;

        if (position < stream.length) {
            cluster = r->next(cluster);
        }
    }

    return bytes;
}

static uint16_t *load_upcase(const ExfatReader *r, Stream stream, uint32_t expected) {
    if (!stream.length || stream.length > 131072 || stream.length % 2) {
        fail("invalid up-case table length");
    }

    claim_stream(r, stream);

    unsigned char *bytes = read_stream(r, stream);
    uint32_t checksum = 0;

    for (size_t i = 0; i < stream.length; i++) {
        checksum = (checksum >> 1 | checksum << 31) + bytes[i];
    }

    if (checksum != expected) {
        fail("up-case table checksum mismatch");
    }

    uint16_t *table = malloc(65536 * sizeof(*table));

    if (!table) {
        fail("out of memory");
    }

    uint32_t code = 0;
    size_t position = 0;

    while (position < stream.length && code < 65536) {
        uint16_t value = u16(bytes + position);
        position += 2;

        /* In an uncompressed table FFFF is the final identity mapping. */
        if (stream.length != 131072 && value == 0xffff && code != 65535) {
            if (position == stream.length) {
                fail("truncated up-case compression run");
            }

            uint16_t run = u16(bytes + position);
            position += 2;

            if (!run || run > 65536 - code) {
                fail("invalid up-case compression run");
            }

            for (uint32_t i = 0; i < run; i++) {
                table[code] = (uint16_t)code;
                code++;
            }
        } else {
            table[code++] = value;
        }
    }

    if (code != 65536 || position != stream.length) {
        fail("incomplete or oversized up-case table");
    }

    /* The spec requires the ASCII mappings; also reject non-idempotent maps. */
    for (uint32_t i = 0; i < 65536; i++) {
        if (table[table[i]] != table[i]) {
            fail("non-idempotent up-case mapping");
        }
    }

    for (uint16_t i = 0; i < 128; i++) {
        uint16_t expected_ascii = i >= 'a' && i <= 'z' ? (uint16_t)(i - 'a' + 'A') : i;

        if (table[i] != expected_ascii) {
            fail("invalid ASCII up-case mapping");
        }
    }

    free(bytes);

    return table;
}

/* File identities bind exact UTF-16 names to the whole directory ancestry.
 * Length-prefixing prevents ambiguous paths; directory order is irrelevant. */
static void hash_identity(const unsigned char parent[32], const unsigned char *set,
                          unsigned char identity[32]) {
    Sha256 hash;
    sha256_init(&hash);
    sha256_update(&hash, parent, 32);

    unsigned char length[2] = {set[32 + 3], 0};
    sha256_update(&hash, length, sizeof(length));
    size_t remaining = (size_t)length[0] * 2;
    size_t index = 2;

    while (remaining) {
        size_t take = remaining < 30 ? remaining : 30;
        sha256_update(&hash, set + index * ENTRY_BYTES + 2, take);
        remaining -= take;
        index++;
    }

    sha256_finish(&hash, identity);
}

/* Hash the bytes a filesystem reader returns, including zero-fill beyond
 * ValidDataLength. Slack and deleted data are not part of a file's contents. */
static void hash_file(const ExfatReader *r, Stream stream, uint64_t valid_length,
                      const unsigned char identity[32]) {
    unsigned char buffer[65536];
    Sha256 hash;
    sha256_init(&hash);
    uint64_t position = 0;
    uint32_t cluster = stream.first;

    while (position < stream.length) {
        uint64_t cluster_position = 0;

        while (cluster_position < r->cluster_bytes && position < stream.length) {
            uint64_t available = stream.length - position;

            if (available > r->cluster_bytes - cluster_position) {
                available = r->cluster_bytes - cluster_position;
            }

            size_t take = available < sizeof(buffer) ? (size_t)available : sizeof(buffer);
            size_t initialized =
                position < valid_length
                    ? (size_t)(valid_length - position < take ? valid_length - position : take)
                    : 0;

            if (initialized) {
                r->read(r->offset(cluster) + cluster_position, buffer, initialized);
            }

            memset(buffer + initialized, 0, take - initialized);
            sha256_update(&hash, buffer, take);
            position += take;
            cluster_position += take;
        }

        if (position < stream.length) {
            cluster = stream.contiguous ? cluster + 1 : r->next(cluster);
        }
    }

    unsigned char digest[32];
    sha256_finish(&hash, digest);
    file_hash_record(r->hashes, identity, digest, stream.length);
}

static void append_directory(Directory **queue, size_t *length, size_t *capacity, Stream stream,
                             const unsigned char identity[32]) {
    if (*length == *capacity) {
        size_t next = *capacity ? *capacity * 2 : 16;

        if (next < *capacity || next > SIZE_MAX / sizeof(**queue)) {
            fail("directory queue too large");
        }

        Directory *grown = realloc(*queue, next * sizeof(**queue));

        if (!grown) {
            fail("out of memory");
        }

        *queue = grown;
        *capacity = next;
    }

    (*queue)[*length].stream = stream;
    memcpy((*queue)[*length].identity, identity, 32);
    (*length)++;
}

static Name validate_file_set(const unsigned char *set, size_t entries, const uint16_t *upcase) {
    uint16_t checksum = 0;

    for (size_t i = 0; i < entries * ENTRY_BYTES; i++) {
        if (i != 2 && i != 3) {
            checksum = rotate16(checksum, set[i]);
        }
    }

    if (checksum != u16(set + 2)) {
        fail("file directory set checksum mismatch");
    }

    const unsigned char *stream = set + ENTRY_BYTES;

    if (entries < 3 || stream[0] != 0xc0 || (stream[1] != 1 && stream[1] != 3)) {
        fail("invalid stream extension");
    }

    if (u16(set + 4) & ~0x37u) {
        fail("unsupported file attributes");
    }

    size_t characters = stream[3];
    size_t names = (characters + 14) / 15;

    if (!characters || entries != 2 + names) {
        fail("unsupported file secondary entries or name length");
    }

    Name name = {0};
    name.length = (uint16_t)characters;
    uint16_t hash = 0;

    for (size_t i = 0; i < names; i++) {
        const unsigned char *entry = set + (i + 2) * ENTRY_BYTES;

        if (entry[0] != 0xc1 || entry[1] != 0) {
            fail("invalid file name entry");
        }

        for (size_t j = 0; j < 15; j++) {
            size_t position = i * 15 + j;
            uint16_t character = u16(entry + 2 + j * 2);

            if (position >= characters) {
                continue;
            }

            if (character < 32 || character == '"' || character == '*' || character == '/' ||
                character == ':' || character == '<' || character == '>' || character == '?' ||
                character == '\\' || character == '|') {
                fail("invalid file name character");
            }

            name.text[position] = upcase[character];
            hash = rotate16(hash, (unsigned char)name.text[position]);
            hash = rotate16(hash, (unsigned char)(name.text[position] >> 8));
        }
    }

    if ((characters == 1 && name.text[0] == '.') ||
        (characters == 2 && name.text[0] == '.' && name.text[1] == '.')) {
        fail("invalid dot directory name");
    }

    if (hash != u16(stream + 4)) {
        fail("file name hash mismatch");
    }

    return name;
}

static int compare_names(const void *left, const void *right) {
    const Name *a = left;
    const Name *b = right;
    size_t shared = a->length < b->length ? a->length : b->length;
    int result = memcmp(a->text, b->text, shared * sizeof(a->text[0]));

    return result ? result : (int)a->length - (int)b->length;
}

void exfat_verify(const ExfatReader *r, uint32_t root) {
    Stream root_stream = {root, 0, 0};

    for (uint32_t c = root; c != UINT32_MAX; c = r->next(c)) {
        if (root_stream.length >= MAX_DIRECTORY_BYTES) {
            fail("root directory exceeds 256 MiB");
        }

        root_stream.length += r->cluster_bytes;
    }

    Cursor root_cursor = cursor(r, root_stream);
    unsigned char entry[ENTRY_BYTES];
    Stream upcase_stream = {0};
    uint32_t table_checksum = 0;

    while (read_entry(&root_cursor, entry) && entry[0]) {
        if (entry[0] == 0x82) {
            if (upcase_stream.first) {
                fail("duplicate up-case table");
            }

            upcase_stream.first = u32(entry + 20);
            upcase_stream.length = u64(entry + 24);
            table_checksum = u32(entry + 4);
        }
    }

    if (!upcase_stream.first) {
        fail("missing up-case table");
    }

    uint16_t *upcase = load_upcase(r, upcase_stream, table_checksum);
    Directory *queue = NULL;
    size_t queue_length = 0;
    size_t queue_capacity = 0;
    uint64_t files = 0;
    uint64_t directories = 0;

    unsigned char root_identity[32] = {0};
    append_directory(&queue, &queue_length, &queue_capacity, root_stream, root_identity);

    for (size_t q = 0; q < queue_length; q++) {
        Cursor directory = cursor(r, queue[q].stream);
        Name *names = NULL;
        size_t name_count = 0;
        size_t name_capacity = 0;
        int label_seen = 0;

        while (read_entry(&directory, entry) && entry[0]) {
            if (!(entry[0] & 0x80)) {
                continue;
            }

            if (entry[0] == 0x81 || entry[0] == 0x82 || entry[0] == 0x83) {
                if (q != 0) {
                    fail("root-only metadata found in subdirectory");
                }

                if (entry[0] == 0x83 && (label_seen++ || entry[1] > 11)) {
                    fail("duplicate or invalid volume label");
                }

                continue;
            }

            if (entry[0] != 0x85) {
                fail("unsupported in-use directory entry type");
            }

            unsigned char set[MAX_SET_BYTES];
            size_t entries = (size_t)entry[1] + 1;
            memcpy(set, entry, ENTRY_BYTES);

            for (size_t i = 1; i < entries; i++) {
                if (!read_entry(&directory, set + i * ENTRY_BYTES)) {
                    fail("truncated file directory set");
                }
            }

            Name name = validate_file_set(set, entries, upcase);

            if (name_count == name_capacity) {
                size_t next = name_capacity ? name_capacity * 2 : 16;

                if (next < name_capacity || next > SIZE_MAX / sizeof(*names)) {
                    fail("too many file names");
                }

                Name *grown = realloc(names, next * sizeof(*names));

                if (!grown) {
                    fail("out of memory");
                }

                names = grown;
                name_capacity = next;
            }

            names[name_count++] = name;

            const unsigned char *extension = set + ENTRY_BYTES;
            Stream stream = {u32(extension + 20), u64(extension + 24), (extension[1] & 2) != 0};
            uint64_t valid_length = u64(extension + 8);
            int is_directory = (u16(set + 4) & 0x10) != 0;

            if (valid_length > stream.length) {
                fail("valid data length exceeds data length");
            }

            if (is_directory &&
                (valid_length != stream.length || stream.length % r->cluster_bytes ||
                 stream.length > MAX_DIRECTORY_BYTES)) {
                fail("invalid directory data length");
            }

            unsigned char identity[32] = {0};

            if (r->hashes) {
                hash_identity(queue[q].identity, set, identity);
            }

            claim_stream(r, stream);

            if (is_directory) {
                directories++;

                if (stream.length) {
                    append_directory(&queue, &queue_length, &queue_capacity, stream, identity);
                }
            } else {
                if (r->hashes) {
                    hash_file(r, stream, valid_length, identity);
                }

                files++;
            }
        }

        if (name_count > 1) {
            qsort(names, name_count, sizeof(*names), compare_names);

            for (size_t i = 1; i < name_count; i++) {
                if (!compare_names(&names[i - 1], &names[i])) {
                    fail("duplicate case-insensitive file name");
                }
            }
        }

        free(names);
    }

    /* Bad-cluster records and vendor allocations are unsupported for now.
     * Fail closed on every allocation that could not be fully accounted for. */
    for (uint32_t i = 0; i < r->cluster_count; i++) {
        if (bit(r->bitmap, i) && !bit(r->owned, i)) {
            fail("allocated clusters are not owned by a supported file or metadata stream");
        }
    }

    printf("Verified files: %" PRIu64 "\nVerified subdirectories: %" PRIu64 "\n", files,
           directories);
    puts("Supported allocation ownership, directory sets, names, and up-case table verified.");

    free(queue);
    free(upcase);
}
