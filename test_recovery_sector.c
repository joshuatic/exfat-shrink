#include "recovery_sector.h"
#include <stdio.h>
#include <string.h>

int main(void) {
    unsigned char before[4096] = {0}, after[4096] = {0}, present[4096] = {0};
    unsigned checks = 0;
    for (unsigned i = 0; i < 8; i++) {
        after[i] = (unsigned char)(i + 1);
    }
    for (unsigned mask = 0; mask < 256; mask++) {
        memcpy(present, before, sizeof(present));
        for (unsigned i = 0; i < 8; i++) {
            if (mask & (1u << i)) {
                present[i] = after[i];
            }
        }
        for (size_t sector = 512; sector <= 4096; sector *= 2) {
            if (!recovery_sector_matches(before, after, present, sector, 0)) {
                return 1;
            }
            checks++;
        }
    }
    memcpy(present, before, sizeof(present));
    for (size_t byte = 0; byte < sizeof(present); byte++) {
        present[byte] = 0xff;
        if (recovery_sector_matches(before, after, present, sizeof(present), 0)) {
            return 1;
        }
        present[byte] = 0;
        checks++;
    }
    present[106] = 2;
    if (!recovery_sector_matches(before, after, present, 512, 1) ||
        recovery_sector_matches(before, after, present, 512, 0)) {
        return 1;
    }
    checks += 2;
    present[106] = 4;
    if (recovery_sector_matches(before, after, present, 512, 1)) {
        return 1;
    }
    checks++;
    printf("%u torn-sector mixture and corruption checks passed.\n", checks);
    return 0;
}
