#ifndef RECOVERY_STATE_H
#define RECOVERY_STATE_H
#include <stddef.h>
#include <stdio.h>
int recovery_state_parse(const char *text, size_t length);
int recovery_state_read(FILE *file, char *buffer, size_t capacity);
#endif
