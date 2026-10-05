#ifndef RECOVERY_SECTOR_H
#define RECOVERY_SECTOR_H
#include <stddef.h>
int recovery_sector_matches(const unsigned char *before, const unsigned char *after,
                            const unsigned char *present, size_t length, int boot);
#endif
