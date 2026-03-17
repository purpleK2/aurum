#ifndef REDIR_H
#define REDIR_H

#include <aurum.h>
#include <ast.h>

typedef struct saved_fd {
    int orig;
    int saved;
    struct saved_fd *next;
} saved_fd_t;

int apply_redirs(shell_t *sh, redir_t *r, saved_fd_t **saved);
void restore_fds(saved_fd_t *s);
void store_heredoc_body(shell_t *sh, int idx, const char *body);

#endif /* REDIR_H */