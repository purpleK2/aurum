#ifndef EXEC_H
#define EXEC_H

#include <aurum.h>
#include <ast.h>

int exec_node(shell_t *sh, node_t *n);
int exec_cmd(shell_t *sh, node_t *n);
int exec_external(shell_t *sh, char **argv, int argc);
#endif /* EXEC_H */