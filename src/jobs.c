#include "aurum.h"

#include "util.h"
#include "sys/wait.h"

#ifdef USE_JOBCONTROL
#include "stdlib.h"
#include "errno.h"


int job_add(shell_t *sh, pid_t pgrp, const char *cmd, int bg) {
    int slot = -1;
    for (int i = 0; i < MAXJOBS; i++) {
        if (!sh->jobs[i].active) { slot = i; break; }
    }
    if (slot < 0) return -1;
    job_t *j = &sh->jobs[slot];
    j->pgrp   = pgrp;
    j->cmd    = cmd ? sh_strdup(cmd) : sh_strdup("?");
    j->state  = J_RUN;
    j->status = 0;
    j->active = 1;
    j->jobno  = slot + 1;
    if (sh->njobs <= slot) sh->njobs = slot + 1;
    if (bg && sh->interactive)
        fprintf(stderr, "[%d] %d\n", j->jobno, (int)pgrp);
    return slot;
}

void job_update(shell_t *sh) {
    pid_t pid;
    int status;
    while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED)) > 0) {
        for (int i = 0; i < sh->njobs; i++) {
            if (!sh->jobs[i].active) continue;
            if (sh->jobs[i].pgrp == pid) {
                sh->jobs[i].status = status;
                if (WIFSTOPPED(status)) sh->jobs[i].state = J_STOP;
                else                    sh->jobs[i].state = J_DONE;
            }
        }
    }
}

void jobs_print(shell_t *sh) {
    job_update(sh);
    for (int i = 0; i < sh->njobs; i++) {
        job_t *j = &sh->jobs[i];
        if (!j->active) continue;
        const char *state = j->state == J_RUN  ? "Running"
                          : j->state == J_STOP ? "Stopped"
                          : "Done";
        printf("[%d]  %-12s %s\n", j->jobno, state, j->cmd);
        if (j->state == J_DONE) {
            free(j->cmd); j->cmd = NULL; j->active = 0;
        }
    }
}

int job_wait(shell_t *sh, pid_t pid) {
    int status = 0;
    pid_t r;
    do {
        r = waitpid(pid, &status, WUNTRACED);
    } while (r < 0 && errno == EINTR);
    if (r < 0) return sh->last_status;
    for (int i = 0; i < sh->njobs; i++) {
        if (sh->jobs[i].active && sh->jobs[i].pgrp == pid) {
            sh->jobs[i].state  = J_DONE;
            sh->jobs[i].status = status;
        }
    }
    if (WIFSIGNALED(status)) {
        int sig = WTERMSIG(status);
        if (sig != SIGPIPE && sh->interactive)
            fprintf(stderr, "aurum: pid %d: signal %d\n", (int)pid, sig);
        return 128 + sig;
    }
    if (WIFSTOPPED(status)) return 128 + WSTOPSIG(status);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

void job_mark_done(shell_t *sh, pid_t pgrp, int status) {
    for (int i = 0; i < sh->njobs; i++) {
        if (sh->jobs[i].active && sh->jobs[i].pgrp == pgrp) {
            sh->jobs[i].state  = J_DONE;
            sh->jobs[i].status = status;
        }
    }
}

#else

int job_add(shell_t *sh, pid_t pgrp, const char *cmd, int bg) {
    (void)sh; (void)cmd;
    sh_error("aurum has not been built with job control!");
    return 0;
}

void job_update(shell_t *sh) {
    int status;
    while (waitpid(-1, &status, WNOHANG) > 0) {}
    (void)sh;
    sh_error("aurum has not been built with job control!");
}

void jobs_print(shell_t *sh) {
    (void)sh;
    sh_error("aurum has not been built with job control!");
}

int job_wait(shell_t *sh, pid_t pid) {
    int status = 0;
    pid_t r;
    do {
        r = waitpid(pid, &status, 0);
    } while (r < 0 && errno == EINTR);
    if (r < 0) return sh->last_status;
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

void job_mark_done(Shell *sh, pid_t pgrp, int status) {
    (void)sh; (void)pgrp; (void)status;
    sh_error("aurum has not been built with job control!");
}

#endif /* USE_JOBCONTROL */