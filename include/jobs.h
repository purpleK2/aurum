#ifndef JOBS_H
#define JOBS_H

#include <aurum.h>

int job_add(shell_t *sh, pid_t pgrp, const char *cmd, int bg);
void job_update(shell_t *sh);
void jobs_print(shell_t *sh);
int job_wait(shell_t *sh, pid_t pid);
void job_mark_done(shell_t *sh, pid_t pgrp, int status);

#endif /* JOBS_H */