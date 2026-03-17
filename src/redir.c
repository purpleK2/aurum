#include "aurum.h"

#include "redir.h"
#include "fcntl.h"
#include "util.h"
#include "unistd.h"
#include "stdlib.h"
#include "vars.h"
#include "errno.h"
#include "expand.h"
#include "string.h"
#include "sys/stat.h"

static int save_fd(int fd, saved_fd_t **list) {
    int saved = fcntl(fd, F_DUPFD, 10);
    if (saved < 0) return -1;
    fcntl(saved, F_SETFD, FD_CLOEXEC);
    saved_fd_t *s = sh_malloc(sizeof *s);
    s->orig = fd;
    s->saved = saved;
    s->next = *list;
    *list = s;
    return saved;
}

void restore_fds(saved_fd_t *s) {
    for (saved_fd_t *cur = s; cur; ) {
        dup2(cur->saved, cur->orig);
        close(cur->saved);
        saved_fd_t *nx = cur->next;
        free(cur);
        cur = nx;
    }
}

static int heredoc_idx_from_placeholder(const char *target) {
    if (!target || target[0] != '\x02') return -1;
    return atoi(target + 1);
}

static char *get_heredoc_body(shell_t *sh, int idx) {
    char key[32];
    snprintf(key, sizeof key, "__heredoc_%d__", idx);
    char *body = var_get(sh, key);
    return body ? sh_strdup(body) : sh_strdup("");
}

void store_heredoc_body(shell_t *sh, int idx, const char *body) {
    char key[32];
    snprintf(key, sizeof key, "__heredoc_%d__", idx);
    var_set(sh, key, body, -1);
}

int apply_redirs(shell_t *sh, redir_t *r, saved_fd_t **saved) {
    for (; r; r = r->next) {
        char *target = NULL;
        int is_here = (r->op == TK_redir_HER || r->op == TK_redir_HED);

        if (is_here) {
            target = sh_strdup(r->target ? r->target : "");
        } else {
            target = expand_word(sh, r->target ? r->target : "");
        }

        int fd = r->fd;

        switch (r->op) {
        case TK_redir_IN: {
            if (saved) save_fd(fd, saved);
            int f = open(target, O_RDONLY);
            if (f < 0) {
                sh_error(sh, "%s: %s", target, strerror(errno));
                free(target); return -1;
            }
            dup2(f, fd); close(f);
            break;
        }
        case TK_redir_OUT: {
            if (saved) save_fd(fd, saved);
            if (sh->opt_C) {
                struct stat st;
                if (stat(target, &st) == 0) {
                    sh_error(sh, "%s: cannot overwrite existing file", target);
                    free(target); return -1;
                }
            }
            int f = open(target, O_WRONLY|O_CREAT|O_TRUNC, 0666);
            if (f < 0) { sh_error(sh, "%s: %s", target, strerror(errno)); free(target); return -1; }
            dup2(f, fd); close(f);
            break;
        }
        case TK_redir_CLOBBER: {
            if (saved) save_fd(fd, saved);
            int f = open(target, O_WRONLY|O_CREAT|O_TRUNC, 0666);
            if (f < 0) { sh_error(sh, "%s: %s", target, strerror(errno)); free(target); return -1; }
            dup2(f, fd); close(f);
            break;
        }
        case TK_redir_APP: {
            if (saved) save_fd(fd, saved);
            int f = open(target, O_WRONLY|O_CREAT|O_APPEND, 0666);
            if (f < 0) { sh_error(sh, "%s: %s", target, strerror(errno)); free(target); return -1; }
            dup2(f, fd); close(f);
            break;
        }
        case TK_redir_HER:
        case TK_redir_HED: {
            if (saved) save_fd(fd, saved);
            int pfd[2];
            if (pipe(pfd) < 0) { free(target); return -1; }
            char *body = expand_word(sh, target);
            size_t bl = strlen(body);
            ssize_t written = write(pfd[1], body, bl);
            (void)written;
            free(body);
            close(pfd[1]);
            dup2(pfd[0], fd); close(pfd[0]);
            break;
        }
        case TK_redir_DUP_IN:
        case TK_redir_DUP_OUT: {
            if (saved) save_fd(fd, saved);
            if (strcmp(target, "-") == 0) {
                close(fd);
            } else {
                int srcfd = atoi(target);
                dup2(srcfd, fd);
            }
            break;
        }
        default:
            break;
        }
        free(target);
    }
    return 0;
}

char *read_heredoc(shell_t *sh, const char *delim, int strip_tabs) {
    size_t cap=256, n=0;
    char *buf = sh_malloc(cap);
    char line[4096];
    while (fgets(line, sizeof line, stdin)) {
        char *check = line;
        if (strip_tabs) while (*check == '\t') check++;
        size_t ll = strlen(check);
        if (ll > 0 && check[ll-1] == '\n') check[ll-1] = '\0';
        if (strcmp(check, delim) == 0) break;
        if (strip_tabs) {
            char *p = line;
            while (*p == '\t') p++;
            size_t pl = strlen(p);
            while (n + pl + 1 >= cap) { cap*=2; buf=sh_realloc(buf,cap); }
            memcpy(buf+n, p, pl); n+=pl;
        } else {
            size_t ll2 = strlen(line);
            while (n + ll2 + 1 >= cap) { cap*=2; buf=sh_realloc(buf,cap); }
            memcpy(buf+n, line, ll2); n+=ll2;
        }
    }
    buf[n] = '\0';
    return buf;
}