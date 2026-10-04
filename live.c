#define _CRT_SECURE_NO_WARNINGS
#include "live.h"
#include "exfat.h"
#include "patch.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#include <share.h>
#include <windows.h>
#include <winioctl.h>

#define BLOCK_BYTES (1024 * 1024)
#define LAYOUT_BYTES (1024 * 1024)

static HANDLE live_volume = INVALID_HANDLE_VALUE;
static HANDLE live_disk = INVALID_HANDLE_VALUE;
static FILE *original;
static uint64_t *changes;
static size_t change_count;
static DWORD sector_bytes;
static PARTITION_INFORMATION_EX selected;
static DRIVE_LAYOUT_INFORMATION_EX *initial_layout;
static DWORD initial_layout_bytes;
static int writes_started;
static int committed;
static char state_path[MAX_PATH];

static void fail(const char *message) {
    fprintf(stderr, "error: %s (Windows error %lu)\n", message, GetLastError());
    exit(1);
}

static void inject_failure(const char *phase) {
#ifdef EXFAT_TEST_FAULTS
    char requested[64];

    if (GetEnvironmentVariableA("EXFAT_TEST_FAULT", requested, sizeof(requested)) &&
        !strcmp(requested, phase)) {
        fail(phase);
    }
#else
    (void)phase;
#endif
}

static int control(HANDLE handle, DWORD code, void *input, DWORD input_bytes, void *output,
                   DWORD output_bytes, DWORD *returned) {
    DWORD ignored;

    return DeviceIoControl(handle, code, input, input_bytes, output, output_bytes,
                           returned ? returned : &ignored, NULL) != 0;
}

static int raw_io(HANDLE handle, uint64_t offset, void *buffer, DWORD length, int writing) {
    LARGE_INTEGER position;
    position.QuadPart = (LONGLONG)offset;
    DWORD done = 0;

    if (!SetFilePointerEx(handle, position, NULL, FILE_BEGIN)) {
        return 0;
    }

    return (writing ? WriteFile(handle, buffer, length, &done, NULL)
                    : ReadFile(handle, buffer, length, &done, NULL)) &&
           done == length;
}

static void read_image(FILE *file, uint64_t offset, void *buffer, size_t length) {
    if (_fseeki64(file, (int64_t)offset, SEEK_SET) || fread(buffer, 1, length, file) != length) {
        fail("cannot read staged image");
    }
}

static void save_state(const char *state) {
    FILE *file = fopen(state_path, "wb");

    if (!file || fprintf(file, "%s\n", state) < 0 || fflush(file) || _commit(_fileno(file))) {
        fail("cannot persist transaction state");
    }

    if (fclose(file)) {
        fail("cannot close transaction state");
    }
}

static DRIVE_LAYOUT_INFORMATION_EX *get_layout(DWORD *bytes) {
    DRIVE_LAYOUT_INFORMATION_EX *layout = calloc(1, LAYOUT_BYTES);

    if (!layout ||
        !control(live_disk, IOCTL_DISK_GET_DRIVE_LAYOUT_EX, NULL, 0, layout, LAYOUT_BYTES, bytes)) {
        free(layout);
        return NULL;
    }

    if (layout->PartitionCount >
        (LAYOUT_BYTES - offsetof(DRIVE_LAYOUT_INFORMATION_EX, PartitionEntry)) /
            sizeof(PARTITION_INFORMATION_EX)) {
        free(layout);
        return NULL;
    }

    return layout;
}

static int same_layout(const DRIVE_LAYOUT_INFORMATION_EX *layout, uint64_t expected_length) {
    if (layout->PartitionStyle != initial_layout->PartitionStyle ||
        layout->PartitionCount != initial_layout->PartitionCount) {
        return 0;
    }

    if (layout->PartitionStyle == PARTITION_STYLE_GPT) {
        if (memcmp(&layout->Gpt.DiskId, &initial_layout->Gpt.DiskId, sizeof(GUID)) ||
            layout->Gpt.StartingUsableOffset.QuadPart !=
                initial_layout->Gpt.StartingUsableOffset.QuadPart ||
            layout->Gpt.UsableLength.QuadPart != initial_layout->Gpt.UsableLength.QuadPart) {
            return 0;
        }
    } else if (layout->Mbr.Signature != initial_layout->Mbr.Signature) {
        return 0;
    }

    int found = 0;

    for (DWORD i = 0; i < layout->PartitionCount; i++) {
        PARTITION_INFORMATION_EX a = layout->PartitionEntry[i];
        PARTITION_INFORMATION_EX b = initial_layout->PartitionEntry[i];
        a.RewritePartition = b.RewritePartition = FALSE;

        if (b.PartitionNumber == selected.PartitionNumber &&
            b.StartingOffset.QuadPart == selected.StartingOffset.QuadPart) {
            if ((uint64_t)a.PartitionLength.QuadPart != expected_length) {
                return 0;
            }

            b.PartitionLength = a.PartitionLength;
            found++;
        }

        /* Compare defined fields, not compiler padding or unused union bytes. */
        if (a.PartitionStyle != b.PartitionStyle || a.PartitionNumber != b.PartitionNumber ||
            a.StartingOffset.QuadPart != b.StartingOffset.QuadPart ||
            a.PartitionLength.QuadPart != b.PartitionLength.QuadPart) {
            return 0;
        }

        if (a.PartitionStyle == PARTITION_STYLE_GPT) {
            if (memcmp(&a.Gpt, &b.Gpt, sizeof(a.Gpt))) {
                return 0;
            }
        } else if (a.PartitionStyle == PARTITION_STYLE_MBR &&
                   memcmp(&a.Mbr, &b.Mbr, sizeof(a.Mbr))) {
            return 0;
        }
    }

    return found == 1;
}

static int resize_partition(int64_t delta) {
    DISK_GROW_PARTITION request;
    memset(&request, 0, sizeof(request));
    request.PartitionNumber = selected.PartitionNumber;
    request.BytesToGrow.QuadPart = delta;

    return control(live_disk, IOCTL_DISK_GROW_PARTITION, &request, sizeof(request), NULL, 0, NULL);
}

static void read_journal(size_t index, int replacement, void *buffer);

/* Process-error recovery uses the journaled sector list and the durable metadata
 * journal. Power loss needs manual recovery; it cannot execute this handler. */
static void cleanup(void) {
    if (writes_started && !committed && live_volume != INVALID_HANDLE_VALUE && original) {
        fprintf(stderr, "Attempting rollback from the durable sector journal...\n");
        DWORD bytes;
        DRIVE_LAYOUT_INFORMATION_EX *layout = get_layout(&bytes);
        uint64_t current_length = 0;

        if (layout) {
            for (DWORD i = 0; i < layout->PartitionCount; i++) {
                if (layout->PartitionEntry[i].PartitionNumber == selected.PartitionNumber &&
                    layout->PartitionEntry[i].StartingOffset.QuadPart ==
                        selected.StartingOffset.QuadPart) {
                    current_length = (uint64_t)layout->PartitionEntry[i].PartitionLength.QuadPart;
                }
            }
        }

        uint64_t old_length = (uint64_t)selected.PartitionLength.QuadPart;
        int restored = layout && current_length && same_layout(layout, current_length);

        if (restored && current_length != old_length) {
            restored = current_length < old_length &&
                       resize_partition((int64_t)(old_length - current_length));
        }

        unsigned char *buffer = malloc(sector_bytes);

        if (!buffer) {
            restored = 0;
        }

        /* Restore non-boot metadata first, and the old boot regions last. */
        for (unsigned pass = 0; restored && pass < 2; pass++) {
            for (size_t i = 0; i < change_count; i++) {
                uint64_t offset = changes[i];

                if ((offset < 24ull * sector_bytes) != (pass == 1)) {
                    continue;
                }

                read_journal(i, 0, buffer);

                if (!raw_io(live_volume, offset, buffer, sector_bytes, 1)) {
                    restored = 0;
                    break;
                }
            }

            if (!FlushFileBuffers(live_disk)) {
                restored = 0;
            }
        }

        unsigned char readback[4096];

        for (size_t i = 0; restored && i < change_count; i++) {
            read_journal(i, 0, buffer);

            if (!raw_io(live_volume, changes[i], readback, sector_bytes, 0) ||
                memcmp(buffer, readback, sector_bytes)) {
                restored = 0;
            }
        }

        DRIVE_LAYOUT_INFORMATION_EX *restored_layout = get_layout(&bytes);
        restored = restored && restored_layout && same_layout(restored_layout, old_length);
        free(restored_layout);

        free(buffer);
        free(layout);
        fprintf(
            stderr,
            restored
                ? "Rollback writes completed. Recheck the volume before reuse.\n"
                : "Rollback incomplete. Keep the recovery directory and restore before reuse.\n");
        FILE *state = fopen(state_path, "wb");

        if (state) {
            fputs(restored
                      ? "ROLLED_BACK: process failure; recheck restored volume before reuse\n"
                      : "RECOVERY_REQUIRED: rollback incomplete; preserve all recovery files\n",
                  state);
            fflush(state);
            _commit(_fileno(state));
            fclose(state);
        }
    }

    if (live_volume != INVALID_HANDLE_VALUE) {
        CloseHandle(live_volume);
        live_volume = INVALID_HANDLE_VALUE;
    }

    if (live_disk != INVALID_HANDLE_VALUE) {
        CloseHandle(live_disk);
        live_disk = INVALID_HANDLE_VALUE;
    }

    if (original) {
        fclose(original);
        original = NULL;
    }

    free(changes);
    changes = NULL;
    free(initial_layout);
    initial_layout = NULL;
}

static void make_path(char output[MAX_PATH], const char *directory, const char *name) {
    if (snprintf(output, MAX_PATH, "%s\\%s", directory, name) >= MAX_PATH) {
        fail("recovery path is too long");
    }
}

static void live_read(void *context, uint64_t offset, void *buffer, size_t length) {
    (void)context;
    unsigned char sector[4096];
    unsigned char *destination = buffer;
    while (length) {
        uint64_t aligned = offset - offset % sector_bytes;
        size_t within = (size_t)(offset - aligned);
        if (!within && length >= sector_bytes) {
            DWORD take = (DWORD)(length > BLOCK_BYTES ? BLOCK_BYTES : length);
            take -= take % sector_bytes;
            if (!raw_io(live_volume, offset, destination, take, 0)) {
                fail("live read failed");
            }
            offset += take;
            destination += take;
            length -= take;
        } else {
            size_t take = sector_bytes - within;
            if (take > length) {
                take = length;
            }
            if (!raw_io(live_volume, aligned, sector, sector_bytes, 0)) {
                fail("live sector read failed");
            }
            memcpy(destination, sector + within, take);
            offset += take;
            destination += take;
            length -= take;
        }
    }
}

static void read_journal(size_t index, int replacement, void *buffer) {
    uint64_t position = 5 * sizeof(uint64_t) + index * (sizeof(uint64_t) + 2ull * sector_bytes) +
                        sizeof(uint64_t) + (replacement ? sector_bytes : 0);
    read_image(original, position, buffer, sector_bytes);
}

int exfat_shrink_live(const char *drive, uint64_t target, const char *directory) {
    if (!drive[0] || drive[1] ||
        !((drive[0] >= 'A' && drive[0] <= 'Z') || (drive[0] >= 'a' && drive[0] <= 'z'))) {
        fail("live drive must be one drive letter");
    }

    char letter = (char)(drive[0] & ~32);
    char device[] = "\\\\.\\X:";
    device[4] = letter;

    char root[] = "X:\\";
    root[0] = letter;
    char windows[MAX_PATH];
    char filesystem[32];

    if (!GetWindowsDirectoryA(windows, MAX_PATH) || windows[0] == letter ||
        !GetVolumeInformationA(root, NULL, 0, NULL, NULL, NULL, filesystem, sizeof(filesystem)) ||
        strcmp(filesystem, "exFAT")) {
        fail("refusing Windows/system or non-exFAT volume");
    }

    if (!target || target % (1024 * 1024)) {
        fail("live target must be a positive multiple of 1 MiB");
    }

    live_volume =
        CreateFileA(device, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                    OPEN_EXISTING, FILE_FLAG_WRITE_THROUGH, NULL);

    if (live_volume == INVALID_HANDLE_VALUE) {
        fail("cannot open live volume; run as administrator");
    }

    atexit(cleanup);
    VOLUME_DISK_EXTENTS extents;

    if (!control(live_volume, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, NULL, 0, &extents,
                 sizeof(extents), NULL) ||
        extents.NumberOfDiskExtents != 1 ||
        !control(live_volume, IOCTL_DISK_GET_PARTITION_INFO_EX, NULL, 0, &selected,
                 sizeof(selected), NULL)) {
        fail("only a single basic partition is supported");
    }

    if (selected.PartitionStyle != PARTITION_STYLE_GPT &&
        selected.PartitionStyle != PARTITION_STYLE_MBR) {
        fail("unsupported partition style");
    }

    if (selected.StartingOffset.QuadPart != extents.Extents[0].StartingOffset.QuadPart ||
        selected.PartitionLength.QuadPart != extents.Extents[0].ExtentLength.QuadPart ||
        target >= (uint64_t)selected.PartitionLength.QuadPart ||
        selected.PartitionLength.QuadPart <= 0) {
        fail("partition extent or requested target is invalid");
    }

    if (selected.PartitionStyle == PARTITION_STYLE_GPT) {
        static const GUID basic = {
            0xebd0a0a2, 0xb9e5, 0x4433, {0x87, 0xc0, 0x68, 0xb6, 0xb7, 0x26, 0x99, 0xc7}};

        if (memcmp(&selected.Gpt.PartitionType, &basic, sizeof(basic)) || selected.Gpt.Attributes) {
            fail(
                "only ordinary GPT basic-data partitions with no special attributes are supported");
        }
    } else if (selected.Mbr.BootIndicator || selected.Mbr.PartitionType != 7) {
        fail("only non-boot MBR type-7 partitions are supported");
    }

    char physical[64];
    snprintf(physical, sizeof(physical), "\\\\.\\PhysicalDrive%lu", extents.Extents[0].DiskNumber);
    live_disk =
        CreateFileA(physical, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, FILE_FLAG_WRITE_THROUGH, NULL);

    DISK_GEOMETRY geometry;

    if (live_disk == INVALID_HANDLE_VALUE || !control(live_disk, IOCTL_DISK_GET_DRIVE_GEOMETRY,
                                                      NULL, 0, &geometry, sizeof(geometry), NULL)) {
        fail("cannot query physical disk geometry");
    }

    sector_bytes = geometry.BytesPerSector;

    if (sector_bytes < 512 || sector_bytes > 4096 || (sector_bytes & (sector_bytes - 1))) {
        fail("unsupported logical sector size");
    }

    initial_layout = get_layout(&initial_layout_bytes);

    if (!initial_layout ||
        !same_layout(initial_layout, (uint64_t)selected.PartitionLength.QuadPart)) {
        fail("disk layout does not match selected volume");
    }

    /* The recovery directory must be new, local, and on another physical disk. */
    char recovery_root[MAX_PATH];

    if (!GetVolumePathNameA(directory, recovery_root, MAX_PATH) || strlen(recovery_root) != 3 ||
        recovery_root[0] == letter) {
        fail("recovery directory must be on a different local disk");
    }

    char recovery_device[] = "\\\\.\\X:";
    recovery_device[4] = recovery_root[0];
    HANDLE recovery_volume = CreateFileA(recovery_device, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                         NULL, OPEN_EXISTING, 0, NULL);
    VOLUME_DISK_EXTENTS recovery_extents;
    int separate = recovery_volume != INVALID_HANDLE_VALUE &&
                   control(recovery_volume, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, NULL, 0,
                           &recovery_extents, sizeof(recovery_extents), NULL) &&
                   recovery_extents.NumberOfDiskExtents == 1 &&
                   recovery_extents.Extents[0].DiskNumber != extents.Extents[0].DiskNumber;

    if (recovery_volume != INVALID_HANDLE_VALUE) {
        CloseHandle(recovery_volume);
    }

    ULARGE_INTEGER free_bytes;
    uint64_t old_length = (uint64_t)selected.PartitionLength.QuadPart;

    if (!separate || !GetDiskFreeSpaceExA(recovery_root, &free_bytes, NULL, NULL) ||
        free_bytes.QuadPart < 1024 * 1024 || !CreateDirectoryA(directory, NULL)) {
        fail("recovery disk space insufficient or recovery directory already exists");
    }

    char journal_path[MAX_PATH];
    make_path(journal_path, directory, "metadata-journal.bin");
    make_path(state_path, directory, "state.txt");
    char layout_path[MAX_PATH];
    make_path(layout_path, directory, "layout-before.bin");
    FILE *layout_file = fopen(layout_path, "wbx");

    if (!layout_file ||
        fwrite(initial_layout, 1, initial_layout_bytes, layout_file) != initial_layout_bytes ||
        fflush(layout_file) || _commit(_fileno(layout_file)) || fclose(layout_file)) {
        fail("cannot save original disk layout");
    }

    printf("Live target %c:, disk %lu, partition %lu, offset %" PRIu64 ", %" PRIu64 " -> %" PRIu64
           " bytes\nRecovery directory: %s\n",
           letter, extents.Extents[0].DiskNumber, selected.PartitionNumber,
           (uint64_t)selected.StartingOffset.QuadPart, old_length, target, directory);
    save_state("PREPARING: no live writes performed");

    if (!control(live_volume, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, NULL) ||
        !control(live_volume, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0, NULL)) {
        fail("cannot lock and dismount selected volume; close its files first");
    }

    if (!FlushFileBuffers(live_disk)) {
        fail("physical disk does not support the required durable flush");
    }

    ExfatLayout filesystem_layout;
    FileHashes hashes = {0};
    puts("Reading and hashing every file directly from the locked volume...");
    if (exfat_validate_reader(live_read, NULL, old_length, 1, target, &filesystem_layout,
                              &hashes) ||
        filesystem_layout.volume_bytes != old_length ||
        filesystem_layout.sector_bytes != sector_bytes) {
        fail("live validation or target check failed; volume unchanged");
    }

    PatchPlan plan = {live_read, NULL, old_length, sector_bytes, NULL, 0, 0};
    exfat_plan_metadata(&filesystem_layout, target, patch_read, patch_write, &plan);
    file_hash_begin_compare(&hashes);
    if (exfat_validate_reader(patch_read, &plan, target, 0, 0, NULL, &hashes) ||
        file_hash_report(&hashes)) {
        fail("planned filesystem or file hashes did not verify; volume unchanged");
    }

    uint64_t journal_bytes =
        5 * sizeof(uint64_t) + plan.count * (sizeof(uint64_t) + 2ull * sector_bytes);
    if (!GetDiskFreeSpaceExA(recovery_root, &free_bytes, NULL, NULL) ||
        free_bytes.QuadPart < journal_bytes + 1024 * 1024) {
        fail("insufficient free space for metadata journal; volume unchanged");
    }
    FILE *journal = _fsopen(journal_path, "wbx", _SH_DENYRW);
    unsigned char *a = malloc(sector_bytes);
    unsigned char *b = malloc(sector_bytes);
    changes = malloc(plan.count * sizeof(*changes));
    if (!journal || !a || !b || !changes) {
        fail("cannot create metadata journal");
    }
    const uint64_t header[5] = {UINT64_C(0x31304a5441465845), sector_bytes, old_length, target,
                                (uint64_t)selected.StartingOffset.QuadPart};
    if (fwrite(header, 1, sizeof(header), journal) != sizeof(header)) {
        fail("journal header write failed");
    }
    for (size_t i = 0; i < plan.count; i++) {
        SectorPatch *item = &plan.items[i];
        changes[change_count++] = item->offset;
        if (fwrite(&item->offset, 1, sizeof(item->offset), journal) != sizeof(item->offset) ||
            fwrite(item->before, 1, sector_bytes, journal) != sector_bytes ||
            fwrite(item->after, 1, sector_bytes, journal) != sector_bytes) {
            fail("journal write failed");
        }
    }
    if (fflush(journal) || _commit(_fileno(journal)) || fclose(journal)) {
        fail("journal flush failed");
    }
    original = _fsopen(journal_path, "rb", _SH_DENYWR);
    if (!original) {
        fail("cannot reopen durable journal");
    }
    /* Verify every record on disk against the validated in-memory plan. */
    for (size_t i = 0; i < plan.count; i++) {
        uint64_t offset;
        read_image(original, sizeof(header) + i * (8 + 2ull * sector_bytes), &offset,
                   sizeof(offset));
        read_journal(i, 0, a);
        read_journal(i, 1, b);
        if (offset != plan.items[i].offset || memcmp(a, plan.items[i].before, sector_bytes) ||
            memcmp(b, plan.items[i].after, sector_bytes)) {
            fail("durable journal readback mismatch");
        }
    }
    printf("Recovery journal: %" PRIu64 " bytes, %zu sectors; no volume images created.\n",
           journal_bytes, change_count);
    patch_free(&plan);

    DWORD layout_bytes;
    DRIVE_LAYOUT_INFORMATION_EX *current = get_layout(&layout_bytes);

    if (!current || !same_layout(current, old_length)) {
        fail("partition layout changed during preparation");
    }

    free(current);
    save_state("JOURNALED: originals and replacements durable; live writes about to begin");
    writes_started = 1;

    /* Mark both boot sectors dirty before any metadata update. */
    for (unsigned i = 0; i < 2; i++) {
        uint64_t offset = (uint64_t)i * 12 * sector_bytes;
        read_journal(i * 12, 0, a);
        a[106] |= 2;

        if (!raw_io(live_volume, offset, a, sector_bytes, 1)) {
            fail("cannot mark volume dirty");
        }
    }

    if (!FlushFileBuffers(live_disk)) {
        fail("cannot flush dirty markers");
    }

    inject_failure("after-dirty");

    save_state("APPLYING: restore from metadata journal after interruption; partition still "
               "original size");

    /* Update non-boot sectors, then backup boot region, then main boot region. */
    for (unsigned pass = 0; pass < 3; pass++) {
        for (size_t i = 0; i < change_count; i++) {
            uint64_t offset = changes[i];
            unsigned region = offset >= 24ull * sector_bytes   ? 0
                              : offset >= 12ull * sector_bytes ? 1
                                                               : 2;

            if (region != pass) {
                continue;
            }

            read_journal(i, 1, b);

            if (!raw_io(live_volume, offset, b, sector_bytes, 1)) {
                fail("live metadata update failed");
            }
        }

        if (!FlushFileBuffers(live_disk)) {
            fail("cannot flush live metadata");
        }
    }

    save_state("FILESYSTEM_WRITTEN: partition still original size; verifying live readback");
    inject_failure("after-filesystem");
    file_hash_begin_compare(&hashes);
    if (exfat_validate_reader(live_read, NULL, target, 0, 0, NULL, &hashes) ||
        file_hash_report(&hashes)) {
        fail("live readback file hashes differ");
    }

    current = get_layout(&layout_bytes);

    if (!current || !same_layout(current, old_length)) {
        fail("partition layout changed before resize");
    }

    free(current);
    save_state(
        "RESIZING_PARTITION: filesystem and readback verified; use layout backup for recovery");

    if (!resize_partition((int64_t)target - (int64_t)old_length)) {
        fail("Windows refused partition shrink");
    }

    inject_failure("after-partition");

    if (!FlushFileBuffers(live_disk)) {
        fail("cannot flush updated partition table");
    }

    current = get_layout(&layout_bytes);

    if (!current || !same_layout(current, target)) {
        fail("partition resize or preservation of neighboring partitions did not verify");
    }

    free(current);
    committed = 1;
    save_state("COMMITTED: selected partition resized and files verified; retain metadata journal");
    file_hash_free(&hashes);
    free(a);
    free(b);
    cleanup();

    printf("Actual partition shrink complete: %c: is now %" PRIu64 " bytes.\n", letter, target);

    return 0;
}

#else

int exfat_shrink_live(const char *drive, uint64_t target, const char *directory) {
    (void)drive;
    (void)target;
    (void)directory;
    fprintf(stderr, "error: live partition shrinking is supported only on Windows\n");

    return 1;
}

#endif
