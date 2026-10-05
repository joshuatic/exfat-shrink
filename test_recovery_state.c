#define _CRT_SECURE_NO_WARNINGS
#include "recovery_state.h"
#include <stdio.h>
#include <string.h>

int main(void) {
    const char *valid[] = {"JOURNALED: ready\n",           "APPLYING: writes\n",
                           "FILESYSTEM_WRITTEN: verify\n", "RESIZING_PARTITION: resize\n",
                           "RECOVERING: restore\n",        "RECOVERY_REQUIRED: restore\n",
                           "ROLLED_BACK: done\n",          "RECOVERED: done\r\n"};
    const char *invalid[] = {"",
                             "APPLYING",
                             "APPLYING: writes",
                             "APPLYING: \n",
                             "APPLYINGX: writes\n",
                             "UNKNOWN: state\n",
                             "COMMITTED: done\n",
                             "PREPARING: no writes\n",
                             "RECOVERED: done\nextra",
                             "APPLYING: writes\nCOMMITTED: done\n",
                             "APPLYING: bad\tstate\n"};
    unsigned checks = 0;
    for (size_t i = 0; i < sizeof(valid) / sizeof(*valid); i++) {
        if (!recovery_state_parse(valid[i], strlen(valid[i]))) {
            return 1;
        }
        checks++;
        for (size_t length = 0; length < strlen(valid[i]); length++) {
            if (recovery_state_parse(valid[i], length)) {
                return 1;
            }
            checks++;
        }
    }
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); i++) {
        if (recovery_state_parse(invalid[i], strlen(invalid[i]))) {
            return 1;
        }
        checks++;
    }
    const char embedded[] = "APPLYING: bad\0state\n";
    if (recovery_state_parse(embedded, sizeof(embedded) - 1)) {
        return 1;
    }
    checks++;
    FILE *file = tmpfile();
    char buffer[256];
    if (!file || fputs(valid[0], file) == EOF || fflush(file) || fseek(file, 0, SEEK_SET) ||
        !recovery_state_read(file, buffer, sizeof(buffer))) {
        return 1;
    }
    fclose(file);
    checks++;
    printf("%u state parsing and truncation checks passed.\n", checks);
    return 0;
}
