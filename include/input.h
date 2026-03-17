#ifndef INPUT_H
#define INPUT_H

#include <aurum.h>

char *read_line(shell_t *sh, const char *prompt);
void history_add(shell_t *sh, const char *line);

#endif /* INPUT_H */