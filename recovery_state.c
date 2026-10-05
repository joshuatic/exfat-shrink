#include "recovery_state.h"
#include <string.h>

int recovery_state_parse(const char *text, size_t length) {
    static const char *const eligible[] = {"JOURNALED",          "APPLYING",   "FILESYSTEM_WRITTEN",
                                           "RESIZING_PARTITION", "RECOVERING", "RECOVERY_REQUIRED",
                                           "ROLLED_BACK",        "RECOVERED"};
    if (!text || !length || length >= 256 || text[length - 1] != '\n') {
        return 0;
    }
    length--;
    if (length && text[length - 1] == '\r') {
        length--;
    }
    for (size_t i = 0; i < length; i++) {
        if ((unsigned char)text[i] < 32 || (unsigned char)text[i] > 126) {
            return 0;
        }
    }
    for (size_t i = 0; i < sizeof(eligible) / sizeof(*eligible); i++) {
        size_t prefix = strlen(eligible[i]);
        if (length > prefix + 2 && !memcmp(text, eligible[i], prefix) && text[prefix] == ':' &&
            text[prefix + 1] == ' ') {
            return 1;
        }
    }
    return 0;
}

int recovery_state_read(FILE *file, char *buffer, size_t capacity) {
    if (!file || !buffer || capacity < 256) {
        return 0;
    }
    size_t length = fread(buffer, 1, capacity - 1, file);
    if (ferror(file) || !feof(file)) {
        return 0;
    }
    buffer[length] = 0;
    return recovery_state_parse(buffer, length);
}
