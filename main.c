#include "exfat.h"
#include "live.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fail(const char *message) {
    fprintf(stderr, "error: %s\n", message);
    exit(1);
}

static void usage(void) {
    puts("Usage: exfat_shrink inspect IMAGE\n"
         "       exfat_shrink plan IMAGE TARGET_BYTES\n"
         "       exfat_shrink shrink-copy SOURCE_IMAGE NEW_IMAGE TARGET_BYTES\n"
         "       exfat_shrink hashes IMAGE\n"
         "       exfat_shrink compare-files SOURCE_IMAGE OTHER_IMAGE\n"
         "       exfat_shrink shrink-live DRIVE_LETTER TARGET_BYTES NEW_RECOVERY_DIRECTORY\n"
         "Image commands require an exFAT boot sector at byte 0. Live mode requires administrator "
         "access.");
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--help")) {
        usage();
        return 0;
    }

    if (argc == 4 && !strcmp(argv[1], "compare-files")) {
        return exfat_compare_files(argv[2], argv[3]);
    }

    if (argc == 3 && !strcmp(argv[1], "hashes")) {
        FileHashes hashes = {0};
        hashes.listing = 1;
        int result = exfat_validate_hashes(argv[2], 0, 0, NULL, &hashes);
        file_hash_free(&hashes);

        return result;
    }

    int live = argc == 5 && !strcmp(argv[1], "shrink-live");
    int shrink = argc == 5 && !strcmp(argv[1], "shrink-copy");
    int plan = argc == 4 && !strcmp(argv[1], "plan");
    if (!live && !shrink && !plan && !(argc == 3 && !strcmp(argv[1], "inspect"))) {
        usage();
        return 2;
    }

    uint64_t target = 0;
    if (plan || shrink || live) {
        for (const char *p = argv[shrink ? 4 : 3]; *p; p++) {
            if (*p < '0' || *p > '9' || target > (UINT64_MAX - (unsigned)(*p - '0')) / 10) {
                fail("target must be decimal bytes");
            }

            target = target * 10 + (unsigned)(*p - '0');
        }

        if (!target) {
            fail("target must be positive");
        }
    }

    if (live) {
        return exfat_shrink_live(argv[2], target, argv[4]);
    }

    if (shrink) {
        return exfat_shrink_copy(argv[2], argv[3], target);
    }

    return exfat_analyze(argv[2], plan, target);
}
