#define _CRT_SECURE_NO_WARNINGS
#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L
#include "exfat.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <io.h>
#include <share.h>
#include <windows.h>
#include <winioctl.h>
#else
#include <sys/file.h>
#include <unistd.h>
#endif

static void fail(const char *message) {
    fprintf(stderr, "error: %s; any incomplete output must not be used\n", message);
    exit(1);
}

static void seek_to(FILE *file, uint64_t offset) {
    if (offset > INT64_MAX) {
        fail("image offset exceeds supported range");
    }

#ifdef _WIN32
    if (_fseeki64(file, (int64_t)offset, SEEK_SET)) {
#else
    if (fseeko(file, (off_t)offset, SEEK_SET)) {
#endif
        fail("image seek failed");
    }
}

static void read_at(FILE *file, uint64_t offset, void *buffer, size_t length) {
    seek_to(file, offset);

    if (fread(buffer, 1, length, file) != length) {
        fail("short source read");
    }
}

static void write_at(FILE *file, uint64_t limit, uint64_t offset, const void *buffer,
                     size_t length) {
    if (offset > limit || length > limit - offset) {
        fail("metadata write would exceed output");
    }

    seek_to(file, offset);

    if (fwrite(buffer, 1, length, file) != length) {
        fail("output write failed");
    }
}

typedef struct {
    FILE *source;
    FILE *output;
    uint64_t limit;
} ImageCopyContext;
static void copy_read(void *context, uint64_t offset, void *buffer, size_t length) {
    ImageCopyContext *copy = context;
    read_at(copy->source, offset, buffer, length);
}
static void copy_write(void *context, uint64_t offset, const void *buffer, size_t length) {
    ImageCopyContext *copy = context;
    write_at(copy->output, copy->limit, offset, buffer, length);
}

int exfat_shrink_copy(const char *source_path, const char *destination_path, uint64_t target) {
    /* Deny competing writers before validating, and keep this handle open
     * through copy and post-write validation. The source handle is read-only. */
#ifdef _WIN32
    FILE *source = _fsopen(source_path, "rb", _SH_DENYWR);
#else
    FILE *source = fopen(source_path, "rb");
#endif

    if (!source) {
        fail("cannot open source exclusively against writers");
    }

#ifndef _WIN32
    if (flock(fileno(source), LOCK_EX | LOCK_NB)) {
        fail("cannot lock source image");
    }
#endif

    ExfatLayout layout;
    FileHashes hashes = {0};

    puts("Hashing every source file before copying...");

    if (exfat_validate_hashes(source_path, 1, target, &layout, &hashes)) {
        fclose(source);
        file_hash_free(&hashes);
        return 1;
    }

    /* C11 exclusive creation refuses existing images, including the source.
     * Device namespace paths are rejected before attempting to create output. */
#ifdef _WIN32
    if (!strncmp(destination_path, "\\\\.\\", 4) || !strncmp(destination_path, "\\\\?\\", 4)) {
        fail("device output paths are not supported");
    }

    FILE *output = _fsopen(destination_path, "wbx", _SH_DENYRW);
#else
    FILE *output = fopen(destination_path, "wbx");
#endif

    if (!output) {
        fail("cannot create new output image; destination must not exist");
    }

#ifdef _WIN32
    struct _stat64 output_stat;

    if (_fstat64(_fileno(output), &output_stat) || (output_stat.st_mode & _S_IFMT) != _S_IFREG) {
#else
    struct stat output_stat;

    if (fstat(fileno(output), &output_stat) || !S_ISREG(output_stat.st_mode)) {
#endif
        fail("output is not a regular image file");
    }

    unsigned char *buffer = malloc(1024 * 1024);

    if (!buffer) {
        fail("out of memory");
    }

    seek_to(source, 0);

#ifdef _WIN32
    DWORD returned;
    HANDLE output_handle = (HANDLE)_get_osfhandle(_fileno(output));

    if (!DeviceIoControl(output_handle, FSCTL_SET_SPARSE, NULL, 0, NULL, 0, &returned, NULL) ||
        _chsize_s(_fileno(output), target)) {
        fail("cannot create sparse output image");
    }
#else
    if (ftruncate(fileno(output), (off_t)target)) {
        fail("cannot size output image");
    }
#endif

    for (uint64_t position = 0; position < target;) {
        size_t length = (size_t)(target - position < 1024 * 1024 ? target - position : 1024 * 1024);

        if (fread(buffer, 1, length, source) != length) {
            fail("image copy failed");
        }

        size_t nonzero = 0;

        while (nonzero < length && buffer[nonzero] == 0) {
            nonzero++;
        }

        if (nonzero < length) {
            write_at(output, target, position, buffer, length);
        }

        position += length;
    }

    ImageCopyContext context = {source, output, target};
    exfat_plan_metadata(&layout, target, copy_read, copy_write, &context);

    if (fflush(output)) {
        fail("cannot flush output");
    }

#ifdef _WIN32
    if (_commit(_fileno(output))) {
#else
    if (fsync(fileno(output))) {
#endif
        fail("cannot durably flush output");
    }

    if (fclose(output)) {
        fail("cannot close output");
    }

    free(buffer);

    puts("Reopening output and hashing every copied file...");
    file_hash_begin_compare(&hashes);
    int result = exfat_validate_hashes(destination_path, 0, 0, NULL, &hashes);
    result |= file_hash_report(&hashes);
    file_hash_free(&hashes);

    if (fclose(source)) {
        fail("cannot close source");
    }

    if (result) {
        return result;
    }

    printf("Created smaller image: %s (%" PRIu64 " bytes). Source unchanged.\n", destination_path,
           target);

    return result;
}
