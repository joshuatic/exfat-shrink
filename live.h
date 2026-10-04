#ifndef LIVE_H
#define LIVE_H

#include <stdint.h>

/* Windows only. Locks the selected volume, journals only changed metadata sectors,
 * stages and verifies an empty-tail shrink, journals sector changes, then
 * changes only that partition's length using the Windows disk API. */
int exfat_shrink_live(const char *drive, uint64_t target_bytes, const char *new_recovery_directory);

#endif
