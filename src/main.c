#include "aurum.h"

#include <unistd.h>
#include "string.h"
#include "util.h"
#include "vars.h"
#include "limits.h"
#include "expand.h"
#ifdef USE_JOBCONTROL
#include <signal.h>
#include "jobs.h"
#endif
#include "input.h"
#include "stdlib.h"
#include "lexer.h"
#include "exec.h"
#include "parser.h"
#include "builtins.h"
#include "errno.h"

static shell_t *init_shell(void) {
    shell_t *sh = sh_malloc(sizeof *sh);
    memset(sh, 0, sizeof *sh);
    sh->ppid    = getppid();
    sh->ifs     = sh_strdup(" \t\n");
    sh->hist_cap = 64;
    sh->history  = sh_malloc(sh->hist_cap * sizeof(char*));
#ifdef USE_JOBCONTROL
    sh->shell_pgid = getpid();
#endif
    import_env(sh);
    char pidbuf[32];
    snprintf(pidbuf, sizeof pidbuf, "%d", (int)getpid());
    var_set(sh, "$",    pidbuf, -1);
    snprintf(pidbuf, sizeof pidbuf, "%d", (int)sh->ppid);
    var_set(sh, "PPID", pidbuf, -1);
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof cwd)) var_set(sh, "PWD", cwd, -1);
    if (!var_get(sh, "PATH"))
        var_set(sh, "PATH", "/bin:/usr/bin:/usr/local/bin", 1);
    if (getegid() == 0) {
        if (!var_get(sh, "PS1"))  var_set(sh, "PS1",  "# ",  -1);
    } else {
        if (!var_get(sh, "PS1"))  var_set(sh, "PS1",  "$ ",  -1);
    }
    if (!var_get(sh, "PS2"))  var_set(sh, "PS2",  "> ",  -1);
    var_set(sh, "#", "0", -1);
    return sh;
}

static void set_positional(shell_t *sh, char **args, int n) {
    var_set(sh, "#", sh_asprintf("%d", n), -1);
    for (int i = 0; i < n; i++) {
        char nm[32]; snprintf(nm, sizeof nm, "%d", i+1);
        var_set(sh, nm, args[i], -1);
    }
}

static char *build_prompt(shell_t *sh) {
    char *ps1 = var_get(sh, "PS1");
    return expand_word(sh, ps1 ? ps1 : "$ ");
}

static int interactive_loop(shell_t *sh) {
    sh->interactive = 1;

#ifdef USE_JOBCONTROL
    signal(SIGINT,  SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGCHLD, SIG_DFL);
    sh->shell_pgid = getpid();
    setpgid(0, sh->shell_pgid);
    tcsetpgrp(0, sh->shell_pgid);
#endif

    char *accum    = sh_strdup("");
    int   accum_len = 0;

    for (;;) {
#ifdef USE_JOBCONTROL
        job_update(sh);
        for (int i = 0; i < sh->njobs; i++) {
            job_t *j = &sh->jobs[i];
            if (j->active && j->state == J_DONE) {
                fprintf(stderr, "[%d]+  Done\t%s\n", j->jobno, j->cmd);
                free(j->cmd); j->cmd = NULL; j->active = 0;
            }
        }
#endif

        char *prompt = accum_len > 0
            ? expand_word(sh, var_get(sh,"PS2") ? var_get(sh,"PS2") : "> ")
            : build_prompt(sh);
        char *line = read_line(sh, prompt);
        free(prompt);

        if (!line) {
            if (accum_len > 0) fputc('\n', stderr);
            free(accum);
            break;
        }

        size_t ll = strlen(line);
        char  *na = sh_malloc(accum_len + ll + 2);
        memcpy(na, accum, accum_len);
        na[accum_len] = '\n';
        memcpy(na + accum_len + 1, line, ll);
        na[accum_len + 1 + ll] = '\0';
        free(accum); accum = na; accum_len = strlen(accum);

        if (sh->opt_v) fputs(line, stderr);

        if (setjmp(err_jmp) != 0) {
            free(accum); accum = sh_strdup(""); accum_len = 0;
            free(line); sh->last_status = 2; continue;
        }

        lexer_t *l = lexer_new(sh, accum, 1);
        node_t *n = parse(sh, l);
        lexer_free(l);

        if (n) {
            history_add(sh, line);
#ifdef USE_JOBCONTROL
            signal(SIGINT, SIG_DFL);
#endif
            int st = exec_node(sh, n);
#ifdef USE_JOBCONTROL
            signal(SIGINT, SIG_IGN);
#endif
            node_free(n);
            sh->last_status = st;
        }

        if (sh->do_exit) {
            free(accum); free(line);
            int code = sh->exit_status;
            trap_run(sh, 0);
            return code;
        }

        free(accum); accum = sh_strdup(""); accum_len = 0;
        free(line);
    }

    trap_run(sh, 0);
    return sh->last_status;
}

static int run_script(shell_t *sh, FILE *f, const char *name) {
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    rewind(f);
    if (fsz < 0) fsz = 0;
    char *src = sh_malloc(fsz + 2);
    long  rd  = (long)fread(src, 1, fsz, f);
    src[rd] = '\0';
    if (sh->opt_v) fputs(src, stderr);

    char *old_name   = sh->script_name;
    sh->script_name  = sh_strdup(name);
    if (setjmp(err_jmp) != 0) {
        free(src); free(sh->script_name);
        sh->script_name = old_name; return 2;
    }
    node_t *n = parse_string(sh, src);
    free(src);
    int ret = n ? exec_node(sh, n) : 0;
    node_free(n);
    if (sh->do_exit) ret = sh->exit_status;
    trap_run(sh, 0);
    free(sh->script_name);
    sh->script_name = old_name;
    return ret;
}

static void usage(const char *name) {
    fprintf(stderr,
        "Usage: %s [options] [script [args...]]\n"
        "       %s [options] -c command [args...]\n"
        "       %s [options] -s [args...]\n"
        "\n"
        "Options:\n"
        "  -c cmd   Execute command string\n"
        "  -s       Read commands from stdin\n"
        "  -i       Force interactive mode\n"
        "  -x       Trace (xtrace)\n"
        "  -e       Exit on error (errexit)\n"
        "  -u       Unset vars are errors (nounset)\n"
        "  -n       No execution (noexec / syntax check)\n"
        "  -v       Verbose (print input)\n"
        "  -f       Disable globbing\n"
        "  -a       Export all variables\n"
        "  --       End of options\n",
        name, name, name);
}

int main(int argc, char **argv) {
    shell_t *sh = init_shell();
    gsh = sh;
    sh->script_name = argv[0];

    int force_interactive = 0;
    int read_stdin        = 0;
    char *cmd_string      = NULL;
    int i;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        if (argv[i][0] != '-' && argv[i][0] != '+') break;
        int on = (argv[i][0] == '-');
        char *flags = argv[i] + 1;
        if (!*flags) break;
        int skip = 0;
        for (char *p = flags; *p && !skip; p++) {
            switch (*p) {
            case 'c': cmd_string = argv[++i]; skip = 1; break;
            case 's': read_stdin = 1;  break;
            case 'i': force_interactive = 1; break;
            case 'x': sh->opt_x = on;  break;
            case 'e': sh->opt_e = on;  break;
            case 'u': sh->opt_u = on;  break;
            case 'n': sh->opt_n = on;  break;
            case 'v': sh->opt_v = on;  break;
            case 'f': sh->opt_f = on;  break;
            case 'a': sh->opt_a = on;  break;
            case 'm': sh->opt_m = on;  break;
            default:
                fprintf(stderr, "aurum: unknown option: -%c\n", *p);
                usage(argv[0]); return 1;
            }
        }
    }

    var_set(sh, "0",
            cmd_string ? "aurum" : (i < argc ? argv[i] : argv[0]), -1);

    if (cmd_string) {
        set_positional(sh, argv+i, argc-i);
        if (setjmp(err_jmp) != 0) return 2;
        node_t *n = parse_string(sh, cmd_string);
        if (!n) return 0;
        int st = exec_node(sh, n);
        node_free(n);
        if (sh->do_exit) st = sh->exit_status;
        trap_run(sh, 0);
        return st;
    }

    if (i < argc && !read_stdin) {
        const char *script = argv[i];
        sh->script_name = sh_strdup(script);
        set_positional(sh, argv+i+1, argc-i-1);
        FILE *f = fopen(script, "r");
        if (!f) {
            fprintf(stderr, "aurum: %s: %s\n", script, strerror(errno));
            return 127;
        }
        {
            char sb[3] = {0};
            if (fread(sb, 1, 2, f) == 2 && sb[0] == '#' && sb[1] == '!') {
                int c; while ((c = fgetc(f)) != EOF && c != '\n') {}
            } else rewind(f);
        }
        int st = run_script(sh, f, script);
        fclose(f);
        return st;
    }

    if (force_interactive || (!read_stdin && isatty(0) && isatty(2))) {
        return interactive_loop(sh);
    }

    return run_script(sh, stdin, "stdin");
}