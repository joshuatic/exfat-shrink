#define _CRT_SECURE_NO_WARNINGS
#include "live.h"
#include "exfat.h"
#include "patch.h"
#include "recovery_sector.h"
#include "recovery_state.h"
#include "sha256.h"
#include <sys/stat.h>

#include <inttypes.h>
#include <errno.h>
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
    char requested[64] = {0};

    if (GetEnvironmentVariableA("EXFAT_TEST_FAULT", requested, sizeof(requested)) &&
        !strcmp(requested, phase)) {
        fail(phase);
    }
    char crash[80];
    snprintf(crash, sizeof(crash), "crash-%s", phase);
    if (!strcmp(requested, "crash-during-rollback") && !strcmp(phase, "after-filesystem")) {
        fail("injected failure before interrupted rollback");
    }
    if (!strcmp(requested, crash)) {
        TerminateProcess(GetCurrentProcess(), 91);
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

static void journal_digest(FILE *file, uint64_t length, unsigned char digest[32]) {
    Sha256 hash;
    sha256_init(&hash);
    sha256_update(&hash, initial_layout, initial_layout_bytes);
    unsigned char buffer[65536];
    for (uint64_t offset = 0; offset < length;) {
        size_t take = (size_t)(length - offset < sizeof(buffer) ? length - offset : sizeof(buffer));
        read_image(file, offset, buffer, take);
        sha256_update(&hash, buffer, take);
        offset += take;
    }
    sha256_finish(&hash, digest);
}

static int persist_state(const char *state) {
    char temporary[MAX_PATH];
    if (snprintf(temporary, sizeof(temporary), "%s.new", state_path) >= (int)sizeof(temporary)) {
        return 0;
    }
    FILE *file = NULL;
    for (unsigned attempt = 0; attempt < 16; attempt++) {
        file = _fsopen(temporary, "wb", _SH_DENYRW);
        if (file || errno != EACCES) {
            break;
        }
        Sleep(200);
    }
    if (!file) {
        return 0;
    }
    int saved = fprintf(file, "%s\n", state) >= 0 && !fflush(file) && !_commit(_fileno(file));
    if (fclose(file)) {
        saved = 0;
    }
    if (!saved) {
        return 0;
    }
    for (unsigned attempt = 0; attempt < 16; attempt++) {
        if (MoveFileExA(temporary, state_path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            return 1;
        }
        DWORD error = GetLastError();
        if (error != ERROR_SHARING_VIOLATION && error != ERROR_ACCESS_DENIED) {
            return 0;
        }
        Sleep(200);
    }
    return 0;
}

static void save_state(const char *state) {
    if (!persist_state(state)) {
        fail("cannot atomically persist transaction state");
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
                inject_failure("during-rollback");
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
        if (!persist_state(
                restored ? "ROLLED_BACK: process failure; recheck restored volume before reuse"
                         : "RECOVERY_REQUIRED: rollback incomplete; preserve all recovery files")) {
            fprintf(stderr, "warning: rollback state publication failed; retain recovery files\n");
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
        5 * sizeof(uint64_t) + plan.count * (sizeof(uint64_t) + 2ull * sector_bytes) + 32;
    if (!GetDiskFreeSpaceExA(recovery_root, &free_bytes, NULL, NULL) ||
        free_bytes.QuadPart < journal_bytes + 1024 * 1024) {
        fail("insufficient free space for metadata journal; volume unchanged");
    }
    inject_failure("before-journal");
    FILE *journal = _fsopen(journal_path, "w+bx", _SH_DENYRW);
    unsigned char *a = malloc(sector_bytes);
    unsigned char *b = malloc(sector_bytes);
    changes = malloc(plan.count * sizeof(*changes));
    if (!journal || !a || !b || !changes) {
        fail("cannot create metadata journal");
    }
    const uint64_t header[5] = {UINT64_C(0x32304a5441465845), sector_bytes, old_length, target,
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
    if (fflush(journal)) {
        fail("journal flush failed");
    }
    unsigned char digest[32];
    journal_digest(journal, journal_bytes - 32, digest);
    if (_fseeki64(journal, (int64_t)(journal_bytes - 32), SEEK_SET) ||
        fwrite(digest, 1, sizeof(digest), journal) != sizeof(digest)) {
        fail("journal checksum write failed");
    }
    if (fflush(journal) || _commit(_fileno(journal)) || fclose(journal)) {
        fail("journal flush failed");
    }
    original = _fsopen(journal_path, "rb", _SH_DENYWR);
    if (!original) {
        fail("cannot reopen durable journal");
    }
    unsigned char stored_digest[32], checked_digest[32];
    read_image(original, journal_bytes - 32, stored_digest, sizeof(stored_digest));
    journal_digest(original, journal_bytes - 32, checked_digest);
    if (memcmp(stored_digest, checked_digest, sizeof(stored_digest))) {
        fail("durable journal checksum mismatch; volume unchanged");
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
    inject_failure("after-journal");
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
    size_t applied = 0;
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

#ifdef EXFAT_TEST_FAULTS
            char fault[64] = {0};
            GetEnvironmentVariableA("EXFAT_TEST_FAULT", fault, sizeof(fault));
            if ((offset == 0 && !strcmp(fault, "torn-boot")) ||
                (offset == 12ull * sector_bytes && !strcmp(fault, "torn-backup"))) {
                read_journal(i, 0, a);
                memcpy(a, b, 84);
                if (!raw_io(live_volume, offset, a, sector_bytes, 1) ||
                    !FlushFileBuffers(live_disk)) {
                    fail("torn boot simulation write failed");
                }
                TerminateProcess(GetCurrentProcess(), 91);
            }
#endif
            if (!raw_io(live_volume, offset, b, sector_bytes, 1)) {
                fail("live metadata update failed");
            }
            applied++;
            if (applied == 1) {
                inject_failure("after-first-write");
            }
            if (applied == change_count / 2) {
                inject_failure("halfway-writes");
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

int exfat_recover_live(const char *drive, const char *directory) {
    if (!drive[0] || drive[1] ||
        !((drive[0] >= 'A' && drive[0] <= 'Z') || (drive[0] >= 'a' && drive[0] <= 'z'))) {
        fail("recovery drive must be one drive letter");
    }
    char letter = (char)(drive[0] & ~32);
    char windows[MAX_PATH];
    if (!GetWindowsDirectoryA(windows, MAX_PATH) || (windows[0] & ~32) == letter) {
        fail("refusing Windows volume recovery");
    }
    char journal_path[MAX_PATH], layout_path[MAX_PATH];
    make_path(journal_path, directory, "metadata-journal.bin");
    make_path(layout_path, directory, "layout-before.bin");
    make_path(state_path, directory, "state.txt");
    FILE *state = _fsopen(state_path, "rb", _SH_DENYWR);
    char status[256] = {0};
    if (!recovery_state_read(state, status, sizeof(status))) {
        fail("missing, invalid, incomplete, or nonrecoverable transaction state");
    }
    fclose(state);
    if (!strncmp(status, "COMMITTED", 9) || !strncmp(status, "PREPARING", 9)) {
        fail("transaction is committed or never began; recovery refused");
    }
    original = _fsopen(journal_path, "rb", _SH_DENYWR);
    FILE *layout_file = _fsopen(layout_path, "rb", _SH_DENYWR);
    struct _stat64 journal_stat, layout_stat;
    if (!original || !layout_file || _fstat64(_fileno(original), &journal_stat) ||
        _fstat64(_fileno(layout_file), &layout_stat) ||
        layout_stat.st_size < (int64_t)offsetof(DRIVE_LAYOUT_INFORMATION_EX, PartitionEntry) ||
        layout_stat.st_size > LAYOUT_BYTES || journal_stat.st_size < 72) {
        fail("invalid recovery file lengths");
    }
    initial_layout_bytes = (DWORD)layout_stat.st_size;
    initial_layout = calloc(1, initial_layout_bytes);
    if (!initial_layout ||
        fread(initial_layout, 1, initial_layout_bytes, layout_file) != initial_layout_bytes) {
        fail("cannot read recovery layout");
    }
    fclose(layout_file);
    if (initial_layout->PartitionCount >
        (initial_layout_bytes - offsetof(DRIVE_LAYOUT_INFORMATION_EX, PartitionEntry)) /
            sizeof(PARTITION_INFORMATION_EX)) {
        fail("recovery layout count exceeds file");
    }
    uint64_t header[5];
    read_image(original, 0, header, sizeof(header));
    sector_bytes = (DWORD)header[1];
    uint64_t old_length = header[2], target = header[3];
    if (header[0] != UINT64_C(0x32304a5441465845) || header[1] != sector_bytes ||
        sector_bytes < 512 || sector_bytes > 4096 || (sector_bytes & (sector_bytes - 1)) ||
        target < 24ull * sector_bytes || target >= old_length || target % (1024 * 1024) ||
        old_length > INT64_MAX || old_length % sector_bytes || header[4] > INT64_MAX) {
        fail("invalid or unsupported recovery journal header");
    }
    uint64_t payload = (uint64_t)journal_stat.st_size - sizeof(header) - 32;
    uint64_t record_bytes = 8 + 2ull * sector_bytes;
    if (payload % record_bytes || payload / record_bytes < 24 || payload > 256ull * 1024 * 1024) {
        fail("invalid recovery record count");
    }
    unsigned char expected[32], actual[32];
    journal_digest(original, (uint64_t)journal_stat.st_size - 32, actual);
    read_image(original, (uint64_t)journal_stat.st_size - 32, expected, sizeof(expected));
    if (memcmp(expected, actual, sizeof(actual))) {
        fail("recovery journal/layout SHA-256 mismatch; no writes performed");
    }
    unsigned found = 0;
    for (DWORD i = 0; i < initial_layout->PartitionCount; i++) {
        PARTITION_INFORMATION_EX *part = &initial_layout->PartitionEntry[i];
        if ((uint64_t)part->StartingOffset.QuadPart == header[4] &&
            (uint64_t)part->PartitionLength.QuadPart == old_length) {
            selected = *part;
            found++;
        }
    }
    if (found != 1 || selected.PartitionStyle != PARTITION_STYLE_GPT) {
        fail("recovery currently requires one uniquely identified GPT partition");
    }
    char device[] = "\\\\.\\X:";
    device[4] = letter;
    live_volume =
        CreateFileA(device, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                    OPEN_EXISTING, FILE_FLAG_WRITE_THROUGH, NULL);
    if (live_volume == INVALID_HANDLE_VALUE) {
        fail("cannot open recovery volume; run as administrator");
    }
    atexit(cleanup);
    VOLUME_DISK_EXTENTS extents;
    if (!control(live_volume, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, NULL, 0, &extents,
                 sizeof(extents), NULL) ||
        extents.NumberOfDiskExtents != 1 ||
        (uint64_t)extents.Extents[0].StartingOffset.QuadPart != header[4] ||
        ((uint64_t)extents.Extents[0].ExtentLength.QuadPart != old_length &&
         (uint64_t)extents.Extents[0].ExtentLength.QuadPart != target)) {
        fail("recovery volume does not match recorded extent");
    }
    char physical[64];
    snprintf(physical, sizeof(physical), "\\\\.\\PhysicalDrive%lu", extents.Extents[0].DiskNumber);
    live_disk =
        CreateFileA(physical, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, FILE_FLAG_WRITE_THROUGH, NULL);
    DISK_GEOMETRY geometry;
    DWORD layout_bytes;
    DRIVE_LAYOUT_INFORMATION_EX *current = get_layout(&layout_bytes);
    if (live_disk == INVALID_HANDLE_VALUE || !current ||
        !same_layout(current, (uint64_t)extents.Extents[0].ExtentLength.QuadPart) ||
        !control(live_disk, IOCTL_DISK_GET_DRIVE_GEOMETRY, NULL, 0, &geometry, sizeof(geometry),
                 NULL) ||
        geometry.BytesPerSector != sector_bytes) {
        fail("recovery disk identity or layout mismatch");
    }
    free(current);
    if (!control(live_volume, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, NULL) ||
        !control(live_volume, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0, NULL) ||
        !FlushFileBuffers(live_disk)) {
        fail("cannot exclusively lock recovery volume and flush disk");
    }
    change_count = (size_t)(payload / record_bytes);
    changes = malloc(change_count * sizeof(*changes));
    PatchPlan restore = {live_read, NULL, old_length, sector_bytes, NULL, 0, 0};
    unsigned char before[4096], after[4096], present[4096];
    if (!changes) {
        fail("out of memory reading recovery records");
    }
    for (size_t i = 0; i < change_count; i++) {
        read_image(original, sizeof(header) + i * record_bytes, &changes[i], sizeof(changes[i]));
        uint64_t offset = changes[i];
        if (offset % sector_bytes || offset > target - sector_bytes ||
            (i && offset <= changes[i - 1]) || (i < 24 && offset != i * sector_bytes)) {
            fail("invalid recovery sector ordering or bounds");
        }
        read_journal(i, 0, before);
        read_journal(i, 1, after);
        live_read(NULL, offset, present, sector_bytes);
        /* Interrupted writes may leave bytewise mixtures of the two recorded
         * versions. Reject bytes outside that set; reconstruct and validate the
         * full original filesystem before writing any recovery sectors. */
        int boot_sector = offset == 0 || offset == 12ull * sector_bytes;
        if (!recovery_sector_matches(before, after, present, sector_bytes, boot_sector)) {
            fail("live sector contains bytes outside journal versions; recovery refused");
        }
        patch_write(&restore, offset, before, sector_bytes);
    }
    FileHashes hashes = {0};
    if (exfat_validate_reader(patch_read, &restore, old_length, 0, 0, NULL, &hashes)) {
        fail("restored filesystem plan invalid; no recovery writes performed");
    }
    patch_free(&restore);
    save_state("RECOVERING: verified journal and disk identity; restoring original partition");
    writes_started = 1;
    /* Reuse the verified rollback path, then reopen for full filesystem/hash checks. */
    HANDLE verification_handle;
    if (!DuplicateHandle(GetCurrentProcess(), live_volume, GetCurrentProcess(),
                         &verification_handle, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
        fail("cannot retain verification handle");
    }
    cleanup();
    live_volume = verification_handle;
    committed = 1;
    state = fopen(state_path, "rb");
    memset(status, 0, sizeof(status));
    if (!state || !fgets(status, sizeof(status), state)) {
        fail("cannot verify rollback state");
    }
    fclose(state);
    if (strncmp(status, "ROLLED_BACK", 11)) {
        fail("rollback did not verify; retain recovery files");
    }
    file_hash_begin_compare(&hashes);
    if (exfat_validate_reader(live_read, NULL, old_length, 0, 0, NULL, &hashes) ||
        file_hash_report(&hashes)) {
        fail("recovery filesystem or hashes failed; retain journal");
    }
    file_hash_free(&hashes);
    save_state(
        "RECOVERED: original partition and metadata restored; current file contents verified");
    cleanup();
    puts("Recovery complete: original partition restored and all current file contents preserved.");
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

int exfat_recover_live(const char *drive, const char *directory) {
    (void)drive;
    (void)directory;
    fprintf(stderr, "error: live recovery is supported only on Windows\n");
    return 1;
}

#endif
