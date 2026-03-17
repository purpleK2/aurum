#include "aurum.h"

#include "util.h"
#include "string.h"
#include "sys/stat.h"
#include "vars.h"
#include "limits.h"
#include "stdlib.h"
#include "unistd.h"
#include "errno.h"
#include "expand.h"
#include "redir.h"
#include "builtins.h"
#include "exec.h"
#include "jobs.h"
#include "parser.h"

static int exec_pipeline(shell_t *sh, node_t *n);
static int exec_simple(shell_t *sh, node_t *n);

static char *find_in_path(shell_t *sh, const char *name) {
    if (strchr(name, '/')) return sh_strdup(name);
    char *path = var_get(sh, "PATH");
    if (!path) path = "/bin:/usr/bin:/usr/local/bin";
    char *pc = sh_strdup(path), *dir = pc, *found = NULL;
    while (dir && *dir) {
        char *col = strchr(dir, ':');
        if (col) *col = '\0';
        char full[PATH_MAX];
        snprintf(full, sizeof full, "%s/%s", *dir ? dir : ".", name);
        struct stat st;
        if (stat(full, &st) == 0 && (st.st_mode & (S_IXUSR|S_IXGRP|S_IXOTH))) {
            found = sh_strdup(full); break;
        }
        dir = col ? col+1 : NULL;
    }
    free(pc);
    return found;
}

int exec_external(shell_t *sh, char **argv, int argc) {
    (void)argc;
    char *path = find_in_path(sh, argv[0]);
    if (!path) { sh_error(sh, "%s: command not found", argv[0]); return 127; }
    char **envp = build_envp(sh);
    execve(path, argv, envp);
    int e = errno;
    free_envp(envp); free(path);
    sh_error(sh, "%s: %s", argv[0], strerror(e));
    return 126;
}

static int exec_simple(shell_t *sh, node_t *n) {
    char **exp_assigns = NULL;
    if (n->nassigns > 0) {
        exp_assigns = sh_malloc(n->nassigns * sizeof(char*));
        for (int i = 0; i < n->nassigns; i++)
            exp_assigns[i] = expand_word(sh, n->assigns[i]);
    }

    int argc = 0;
    char **argv = NULL;
    if (n->argc > 0)
        argv = expand_words(sh, n->argv, n->argc, &argc);

    if (argc == 0) {
        saved_fd_t *saved = NULL;
        if (n->redirs && apply_redirs(sh, n->redirs, &saved) < 0) {
            restore_fds(saved);
            for (int i=0;i<n->nassigns;i++) free(exp_assigns?exp_assigns[i]:NULL);
            free(exp_assigns); return 1;
        }
        for (int i = 0; i < n->nassigns; i++) {
            char *eq = strchr(exp_assigns[i], '=');
            if (eq) {
                char *nm = sh_strndup(exp_assigns[i], eq - exp_assigns[i]);
                var_set(sh, nm, eq+1, sh->opt_a ? 1 : -1); free(nm);
            }
            free(exp_assigns[i]);
        }
        free(exp_assigns);
        if (saved) restore_fds(saved);
        return 0;
    }

    if (sh->opt_x) {
        fprintf(stderr, "+ ");
        for (int i=0;i<n->nassigns;i++) if(exp_assigns) fprintf(stderr,"%s ",exp_assigns[i]);
        for (int i=0;i<argc;i++) fprintf(stderr,"%s ",argv[i]);
        fputc('\n', stderr);
    }

    func_t *fn = NULL;
    for (func_t *f = sh->funcs; f; f = f->next)
        if (strcmp(f->name, argv[0]) == 0) { fn = f; break; }

    if (fn) {
        saved_fd_t *saved = NULL;
        if (n->redirs && apply_redirs(sh, n->redirs, &saved) < 0) {
            restore_fds(saved); goto cleanup;
        }
        char *old_np = var_get(sh,"#"); int old_n = old_np ? atoi(old_np) : 0;
        char **old_pos = sh_malloc((old_n+1)*sizeof(char*));
        for (int i=1;i<=old_n;i++) {
            char nm[32]; snprintf(nm,sizeof nm,"%d",i);
            char *v=var_get(sh,nm); old_pos[i-1]=sh_strdup(v?v:"");
        }
        old_pos[old_n]=NULL;
        for (int i=1;i<argc;i++) {
            char nm[32]; snprintf(nm,sizeof nm,"%d",i);
            var_set(sh,nm,argv[i],-1);
        }
        var_set(sh,"#",sh_asprintf("%d",argc-1),-1);
        var_t *old_locals = sh->locals; sh->locals = NULL;
        for (int i=0;i<n->nassigns;i++) {
            char *eq=strchr(exp_assigns[i],'=');
            if(eq){ char *nm=sh_strndup(exp_assigns[i],eq-exp_assigns[i]);
                var_set(sh,nm,eq+1,-1); free(nm); }
            free(exp_assigns[i]);
        }
        free(exp_assigns); exp_assigns=NULL;

        int ret = exec_node(sh, fn->body);

        for (var_t *v=sh->locals,*nx; v; v=nx) { nx=v->next; free(v->name);free(v->val);free(v); }
        sh->locals = old_locals;
        for (int i=1;i<=old_n;i++) {
            char nm[32]; snprintf(nm,sizeof nm,"%d",i);
            var_set(sh,nm,old_pos[i-1],-1); free(old_pos[i-1]);
        }
        free(old_pos);
        var_set(sh,"#",sh_asprintf("%d",old_n),-1);
        if (saved) restore_fds(saved);
        if (sh->do_return) { ret=sh->returning; sh->do_return=0; sh->returning=0; }
        for (int i=0;i<argc;i++) free(argv[i]); free(argv);
        return ret;
    }

    if (is_builtin(argv[0])) {
        saved_fd_t *saved = NULL;
        if (n->redirs && apply_redirs(sh, n->redirs, &saved) < 0) {
            restore_fds(saved); goto cleanup;
        }
        for (int i=0;i<n->nassigns;i++) {
            char *eq=strchr(exp_assigns[i],'=');
            if(eq){ char *nm=sh_strndup(exp_assigns[i],eq-exp_assigns[i]);
                var_set(sh,nm,eq+1,-1); free(nm); }
            free(exp_assigns[i]);
        }
        free(exp_assigns); exp_assigns=NULL;
        int ret = exec_builtin(sh, argv, argc);
        if (saved) restore_fds(saved);
        for (int i=0;i<argc;i++) free(argv[i]); free(argv);
        return ret;
    }

    if (sh->opt_n) goto cleanup;

    pid_t pid = fork();
    if (pid < 0) { sh_error(sh,"fork: %s",strerror(errno)); goto cleanup; }
    if (pid == 0) {
        for (int i=0;i<n->nassigns;i++) {
            char *eq=strchr(exp_assigns[i],'=');
            if(eq){ char *nm=sh_strndup(exp_assigns[i],eq-exp_assigns[i]);
                var_set(sh,nm,eq+1,1); free(nm); }
        }
        if (n->redirs && apply_redirs(sh, n->redirs, NULL) < 0) exit(1);
        exec_external(sh, argv, argc);
        exit(127);
    }

    for (int i=0;i<n->nassigns;i++) free(exp_assigns?exp_assigns[i]:NULL);
    free(exp_assigns);
    for (int i=0;i<argc;i++) free(argv[i]); free(argv);
    int st = job_wait(sh, pid);
    sh->last_status = st;
    return st;

cleanup:
    for (int i=0;i<n->nassigns;i++) if(exp_assigns) free(exp_assigns[i]);
    free(exp_assigns);
    for (int i=0;i<argc;i++) if(argv) free(argv[i]); free(argv);
    return sh->last_status;
}

static int exec_pipeline(shell_t *sh, node_t *n) {
    int ncmds = n->ncmds;
    if (ncmds == 1 && !n->pipe_bang)
        return exec_node(sh, n->cmds[0]);

    int (*fds)[2] = sh_malloc((ncmds-1) * sizeof(int[2]));
    for (int i = 0; i < ncmds-1; i++) {
        if (pipe(fds[i]) < 0) {
            sh_error(sh,"pipe: %s",strerror(errno)); free(fds); return 1;
        }
    }

    pid_t *pids = sh_malloc(ncmds * sizeof(pid_t));
    for (int i = 0; i < ncmds; i++) {
        pid_t pid = fork();
        if (pid < 0) { sh_error(sh,"fork: %s",strerror(errno)); pids[i]=-1; continue; }
        if (pid == 0) {
            if (i > 0)        dup2(fds[i-1][0], 0);
            if (i < ncmds-1)  dup2(fds[i][1], 1);
            for (int j=0;j<ncmds-1;j++) { close(fds[j][0]); close(fds[j][1]); }
            free(fds); free(pids);
            int st = exec_node(sh, n->cmds[i]);
            exit(st);
        }
        pids[i] = pid;
    }
    for (int i=0;i<ncmds-1;i++) { close(fds[i][0]); close(fds[i][1]); }
    free(fds);

    int last_status = 0;
    for (int i=0; i<ncmds; i++) {
        if (pids[i] > 0) {
            int st = job_wait(sh, pids[i]);
            if (i == ncmds-1) last_status = st;
        }
    }
    free(pids);

    if (n->pipe_bang) last_status = !last_status;
    sh->last_status = last_status;
    return last_status;
}

int exec_node(shell_t *sh, node_t *n) {
    if (!n) return 0;
    if (sh->do_exit || sh->do_return) return sh->last_status;

    int ret = 0;
    switch (n->type) {

    case N_CMD:
        ret = exec_simple(sh, n);
        break;

    case N_PIPE:
        ret = exec_pipeline(sh, n);
        break;

    case N_LIST:
        if (n->op == TK_AMP) {
            pid_t pid = fork();
            if (pid == 0) {
                int st = exec_node(sh, n->left);
                exit(st);
            }
            if (pid > 0) {
                char *cmd = (n->left->type == N_CMD && n->left->argc > 0)
                            ? n->left->argv[0] : "?";
#ifdef USE_JOBCONTROL
                job_add(sh, pid, cmd, 1);
#else
                job_add(sh, pid, cmd, 1);
#endif
                sh->last_status = 0; ret = 0;
            }
            if (n->right) ret = exec_node(sh, n->right);
        } else {
            ret = exec_node(sh, n->left);
            sh->last_status = ret;
            if (sh->do_exit || sh->do_return || sh->breaking || sh->continuing)
                return ret;
            if (n->right) ret = exec_node(sh, n->right);
        }
        break;

    case N_AND_OR:
        ret = exec_node(sh, n->left);
        sh->last_status = ret;
        if (sh->do_exit || sh->do_return) return ret;
        if (n->op == TK_AND) { if (ret == 0) ret = exec_node(sh, n->right); }
        else                 { if (ret != 0) ret = exec_node(sh, n->right); }
        break;

    case N_BRACE:
        if (n->redirs) {
            saved_fd_t *saved = NULL;
            if (apply_redirs(sh, n->redirs, &saved) < 0) { ret=1; break; }
            ret = exec_node(sh, n->inner);
            if (saved) restore_fds(saved);
        } else {
            ret = exec_node(sh, n->inner);
        }
        break;

    case N_SUBSHELL: {
        pid_t pid = fork();
        if (pid < 0) { sh_error(sh,"fork: %s",strerror(errno)); ret=1; break; }
        if (pid == 0) {
            if (n->redirs && apply_redirs(sh, n->redirs, NULL) < 0) exit(1);
            shell_t sub; memcpy(&sub, sh, sizeof sub);
            sub.subshell = 1;
            int st = exec_node(&sub, n->inner);
            exit(st);
        }
        ret = job_wait(sh, pid);
        break;
    }

    case N_BANG:
        ret = exec_node(sh, n->inner);
        ret = !ret;
        sh->last_status = ret;
        break;

    case N_IF: {
        int cond = exec_node(sh, n->cond);
        sh->last_status = cond;
        if (sh->do_exit || sh->do_return) return sh->last_status;
        if (cond == 0) ret = exec_node(sh, n->body);
        else if (n->els) ret = exec_node(sh, n->els);
        else ret = 0;
        break;
    }

    case N_WHILE:
        ret = 0;
        while (1) {
            int cond = exec_node(sh, n->cond);
            if (sh->do_exit || sh->do_return) return sh->last_status;
            if (cond != 0) break;
            ret = exec_node(sh, n->body);
            if (sh->do_exit || sh->do_return) return ret;
            if (sh->breaking)   { sh->breaking--;   break;    }
            if (sh->continuing) { sh->continuing--;
                                  if (sh->continuing) break; }
        }
        break;

    case N_UNTIL:
        ret = 0;
        while (1) {
            int cond = exec_node(sh, n->cond);
            if (sh->do_exit || sh->do_return) return sh->last_status;
            if (cond == 0) break;
            ret = exec_node(sh, n->body);
            if (sh->do_exit || sh->do_return) return ret;
            if (sh->breaking)   { sh->breaking--;   break;    }
            if (sh->continuing) { sh->continuing--;
                                  if (sh->continuing) break; }
        }
        break;

    case N_FOR: {
        ret = 0;
        char **words; int nw;
        if (n->nwords == 0) {
            char *np = var_get(sh,"#"); nw = np ? atoi(np) : 0;
            words = sh_malloc((nw+1)*sizeof(char*));
            for (int i=1;i<=nw;i++) {
                char nm[32]; snprintf(nm,sizeof nm,"%d",i);
                char *v=var_get(sh,nm); words[i-1]=sh_strdup(v?v:"");
            }
            words[nw]=NULL;
        } else {
            words = expand_words(sh, n->words, n->nwords, &nw);
        }
        for (int i=0; i<nw; i++) {
            var_set(sh, n->name, words[i], -1);
            ret = exec_node(sh, n->body);
            if (sh->do_exit || sh->do_return) break;
            if (sh->breaking)   { sh->breaking--;   break;    }
            if (sh->continuing) { sh->continuing--;
                                  if (sh->continuing) break; }
        }
        for (int i=0;i<nw;i++) free(words[i]); free(words);
        break;
    }

    case N_CASE: {
        char *word = expand_word(sh, n->name);
        ret = 0;
        for (node_t *item = n->cases; item; item = item->next_case) {
            int matched = 0;
            for (int i=0; i<item->npats; i++) {
                const char *raw = item->patterns[i];
                char *pat = expand_word(sh, raw);
                int raw_quoted = 0;
                for (const char *c = raw; *c; c++)
                    if (*c == '"' || *c == '\'' || (unsigned char)*c == 0x01 || *c == '\\')
                        { raw_quoted=1; break; }
                if (raw_quoted) {
                    size_t plen = strlen(pat);
                    char *epat = sh_malloc(plen*2+1);
                    char *d = epat;
                    for (char *s = pat; *s; s++) {
                        if (*s == '*' || *s == '?' || *s == '[' || *s == '\\')
                            *d++ = '\\';
                        *d++ = *s;
                    }
                    *d = '\0';
                    free(pat); pat = epat;
                }
                if (pattern_match(pat, word)) matched = 1;
                free(pat);
                if (matched) break;
            }
            if (matched) {
                ret = item->action ? exec_node(sh, item->action) : 0;
                break;
            }
        }
        free(word);
        break;
    }

    case N_FUNC: {
        for (func_t **fp=&sh->funcs; *fp; fp=&(*fp)->next) {
            if (!strcmp((*fp)->name, n->name)) {
                func_t *old=*fp; *fp=old->next;
                free(old->name); node_free(old->body); free(old);
                break;
            }
        }
        func_t *fn = sh_malloc(sizeof *fn);
        fn->name = sh_strdup(n->name);
        fn->body = n->func_body;
        n->func_body = NULL;
        fn->next = sh->funcs;
        sh->funcs = fn;
        ret = 0;
        break;
    }

    case N_redir: {
        saved_fd_t *saved = NULL;
        if (apply_redirs(sh, n->redirs, &saved) < 0) { ret=1; break; }
        ret = exec_node(sh, n->inner);
        if (saved) restore_fds(saved);
        break;
    }

    default:
        sh_error(sh, "exec: unknown node type %d", n->type);
        ret = 1;
    }

    sh->last_status = ret;
    if (sh->opt_e && ret != 0 && !sh->do_exit && !sh->do_return) {
        sh->do_exit    = 1;
        sh->exit_status = ret;
    }
    return ret;
}

int exec_cmd(shell_t *sh, node_t *n) { return exec_node(sh, n); }