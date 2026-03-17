#ifndef SHELL_H
#define SHELL_H

#include <ast.h>

#include <stdio.h>
#include <sys/types.h>
#include <setjmp.h>

// this has to be here because of circular includes
typedef struct var {
    char *name;
    char *val;
    int exported;
    int readonly;
    struct var *next;
} var_t;

typedef struct func {
    char *name;
    node_t *body;
    struct func *next;
} func_t;

#ifdef USE_JOBCONTROL
#define MAXJOBS 64
typedef enum { J_RUN, J_STOP, J_DONE } job_state;
typedef struct job {
    pid_t pgrp;
    char *cmd;
    job_state state;
    int status;
    int jobno;
    int active;
} job_t;
#endif /* USE_JOBCONTROL */

typedef struct shell {
    var_t *vars;
    var_t *locals;

    func_t *funcs;

    int opt_x, opt_e, opt_u, opt_n, opt_v;
    int opt_f, opt_a, opt_m, opt_C;

    int last_status;
    int interactive;
    int subshell;
    int depth;
    char *script_name;
    int lineno;

    FILE *input;
    char *input_buf;
    size_t input_len;
    size_t input_pos;

#ifdef USE_JOBCONTROL
    job_t jobs[MAXJOBS]; // todo: linked list maybe instead of fixed array? or hashmaps since pids have to be unique?
    int njobs;
    pid_t shell_pgid;
#endif

    int breaking;
    int continuing;
    int returning;
    int do_return;
    int do_exit;
    int exit_status;

    char *ifs;
    pid_t ppid;

    char **history;
    int hist_size;
    int hist_cap;
} shell_t;

extern shell_t *gsh;
extern jmp_buf err_jmp;

#endif /* SHELL_H */