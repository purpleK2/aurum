#ifndef BUILTINS_H
#define BUILTINS_H

#include <aurum.h>

int exec_builtin(shell_t *sh, char **argv, int argc);
int is_builtin(const char *name);
void trap_run(shell_t *sh, int sig);

#endif /* BUILTINS_H */