#ifndef LEXER_H
#define LEXER_H

#include <aurum.h>
#include <token.h>

typedef struct lexer {
    shell_t *sh;

    char *buf;
    size_t len;
    size_t pos;

    int interactive;
    int lineno;

    struct here_entry {
        char *delim;
        int strip_tabs;
        char *body;
    } here[16];
    int nhere;

    token_t *peeked;

    int need_more;
    int start_of_cmd;
} lexer_t;

lexer_t *lexer_new(shell_t *sh, const char *src, int interactive);
void lexer_free(lexer_t *l);
token_t *lexer_next(lexer_t *l);
token_t *lexer_peek(lexer_t *l);
void token_free(token_t *t);
char *token_type_str(token_type t);

void lexer_push_heredoc(lexer_t *l, const char *delim, int strip_tabs);
char *lexer_get_heredoc(lexer_t *l, int i);
int lexer_heredoc_count(lexer_t *l);

#endif /* LEXER_H */