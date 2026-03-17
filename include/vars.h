#ifndef VARS_H
#define VARS_H

#include <aurum.h>

var_t *var_find(shell_t *sh, const char *name);
char *var_get(shell_t *sh, const char *name);
void var_set(shell_t *sh, const char *name, const char *val, int export_flag);
void var_unset(shell_t *sh, const char *name);
char *var_special(shell_t *sh, const char *name);
char **build_envp(shell_t *sh);
void free_envp(char **envp);
void import_env(shell_t *sh);

#endif /* VARS_H */