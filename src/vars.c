#include "aurum.h"

#include "string.h"
#include "util.h"
#include "stdlib.h"
#include "unistd.h"

var_t *var_find(shell_t *sh, const char *name) {
    for (var_t *v = sh->locals; v; v = v->next)
        if (strcmp(v->name, name) == 0) return v;
    for (var_t *v = sh->vars; v; v = v->next)
        if (strcmp(v->name, name) == 0) return v;
    return NULL;
}

char *var_get(shell_t *sh, const char *name) {
    var_t *v = var_find(sh, name);
    return v ? v->val : NULL;
}

void var_set(shell_t *sh, const char *name, const char *val, int export_flag) {
    var_t *v = var_find(sh, name);
    if (v && v->readonly) {
        sh_error(sh, "%s: is read only", name);
        return;
    }
    if (!v) {
        v = sh_malloc(sizeof *v);
        v->name = sh_strdup(name);
        v->val = NULL;
        v->exported = 0;
        v->readonly = 0;
        v->next = sh->vars;
        sh->vars = v;
    }
    free(v->val);
    v->val = val ? sh_strdup(val) : sh_strdup("");
    if (export_flag > 0) v->exported = 1;
    if (sh->opt_a && export_flag >= 0) v->exported = 1;
}

void var_unset(shell_t *sh, const char *name) {
    var_t **pp = &sh->vars;
    for (; *pp; pp = &(*pp)->next) {
        if (strcmp((*pp)->name, name) == 0) {
            var_t *v = *pp;
            if (v->readonly) { sh_error(sh, "%s: is read only", name); return; }
            *pp = v->next;
            free(v->name); free(v->val); free(v);
            return;
        }
    }
    pp = &sh->locals;
    for (; *pp; pp = &(*pp)->next) {
        if (strcmp((*pp)->name, name) == 0) {
            var_t *v = *pp;
            *pp = v->next;
            free(v->name); free(v->val); free(v);
            return;
        }
    }
}

char *var_special(shell_t *sh, const char *name) {
    if (strcmp(name, "?") == 0)
        return sh_asprintf("%d", sh->last_status);
    if (strcmp(name, "$") == 0)
        return sh_asprintf("%d", (int)getpid());
    if (strcmp(name, "PPID") == 0)
        return sh_asprintf("%d", (int)sh->ppid);
    if (strcmp(name, "!") == 0) {
#ifdef USE_JOBCONTROL
        for (int i = sh->njobs-1; i >= 0; i--)
            if (sh->jobs[i].active)
                return sh_asprintf("%d", (int)sh->jobs[i].pgrp);
#endif
        return sh_strdup("0");
    }
    if (strcmp(name, "LINENO") == 0)
        return sh_asprintf("%d", sh->lineno);
    if (strcmp(name, "-") == 0) {
        char buf[32]; int n=0;
        if (sh->opt_x) buf[n++]='x';
        if (sh->opt_e) buf[n++]='e';
        if (sh->opt_u) buf[n++]='u';
        if (sh->opt_n) buf[n++]='n';
        if (sh->opt_v) buf[n++]='v';
        if (sh->opt_f) buf[n++]='f';
        if (sh->opt_a) buf[n++]='a';
        if (sh->opt_m) buf[n++]='m';
        if (sh->opt_C) buf[n++]='C';
        buf[n]='\0';
        return sh_strdup(buf);
    }
    return NULL;
}

char **build_envp(shell_t *sh) {
    int count = 0;
    for (var_t *v = sh->vars; v; v = v->next)
        if (v->exported) count++;
    char **envp = sh_malloc((count+1) * sizeof(char*));
    int i = 0;
    for (var_t *v = sh->vars; v; v = v->next) {
        if (v->exported) {
            envp[i++] = sh_asprintf("%s=%s", v->name, v->val ? v->val : "");
        }
    }
    envp[i] = NULL;
    return envp;
}

void free_envp(char **envp) {
    if (!envp) return;
    for (int i = 0; envp[i]; i++) free(envp[i]);
    free(envp);
}

void import_env(shell_t *sh) {
    extern char **environ;
    for (char **e = environ; e && *e; e++) {
        char *eq = strchr(*e, '=');
        if (!eq) continue;
        char *name = sh_strndup(*e, eq - *e);
        char *val  = eq + 1;
        var_t *v = sh_malloc(sizeof *v);
        v->name = name;
        v->val  = sh_strdup(val);
        v->exported = 1;
        v->readonly = 0;
        v->next = sh->vars;
        sh->vars = v;
    }
}