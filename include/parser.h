#ifndef PARSER_H
#define PARSER_H

#include <aurum.h>
#include <lexer.h>
#include <ast.h>

typedef struct parer {
    shell_t *sh;
    lexer_t *l;
    token_t *cur;
    token_t *ahead;
} parser_t;

node_t *parse(shell_t *sh, lexer_t *l);
node_t *parse_string(shell_t *sh, const char *s);
void node_free(node_t *n);

#endif /* PARSER_H */