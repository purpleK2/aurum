#ifndef AST_H
#define AST_H

#include <token.h>

typedef enum {
    N_CMD, N_PIPE, N_LIST, N_AND_OR, N_redir,
    N_IF, N_WHILE, N_UNTIL, N_FOR, N_CASE, N_CASE_ITEM,
    N_SUBSHELL, N_BRACE, N_FUNC, N_BANG
} node_type;

typedef struct redir {
    int fd;
    token_type op;
    char *target;
    struct redir *next;
} redir_t;

typedef struct node {
    node_type type;

    char **argv;
    int argc;
    char **assigns;
    int nassigns;
    redir_t *redirs;

    struct node **cmds;
    int ncmds;
    int pipe_bang;

    struct node *left;
    struct node *right;
    token_type op;

    struct node *cond;
    struct node *body;
    struct node *els;

    char *name;
    char **words;
    int nwords;
    struct node *cases;

    char **patterns;
    int npats;
    struct node *action;
    struct node *next_case;

    struct node *func_body;

    struct node *inner;
} node_t;

#endif /* AST_H */