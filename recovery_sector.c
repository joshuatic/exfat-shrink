#include "recovery_sector.h"

int recovery_sector_matches(const unsigned char *before, const unsigned char *after,
                            const unsigned char *present, size_t length, int boot) {
    if (!before || !after || !present || length < 512 || length > 4096 || (length & (length - 1))) {
        return 0;
    }
    for (size_t byte = 0; byte < length; byte++) {
        if (present[byte] == before[byte] || present[byte] == after[byte]) {
            continue;
        }
        if (!boot || byte != 106 ||
            (present[byte] != (unsigned char)(before[byte] | 2) &&
             present[byte] != (unsigned char)(after[byte] | 2))) {
            return 0;
        }
    }
    return 1;
}
