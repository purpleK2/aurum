#include "aurum.h"

#include "dirent.h"
#include "pwd.h"
#include "expand.h"
#include "string.h"
#include "util.h"
#include "stdlib.h"
#include "ctype.h"
#include "vars.h"
#include "parser.h"
#include "unistd.h"
#include "sys/wait.h"
#include "exec.h"

static char *expand_one(shell_t *sh, const char *s, int in_dquote);
static int   arith_eval(shell_t *sh, const char *expr, long *result);

int pattern_match(const char *pat, const char *str) {
    for (;;) {
        if (!*pat) return !*str;
        if (*pat == '*') {
            while (*pat == '*') pat++;
            if (!*pat) return 1;
            for (; *str; str++)
                if (pattern_match(pat, str)) return 1;
            return 0;
        }
        if (!*str) return 0;
        if (*pat == '?') { pat++; str++; continue; }
        if (*pat == '[') {
            pat++;
            int negate = 0, matched = 0;
            if (*pat == '!' || *pat == '^') { negate = 1; pat++; }
            if (*pat == ']') { if (*str == ']') matched=1; pat++; }
            while (*pat && *pat != ']') {
                if (pat[1] == '-' && pat[2] && pat[2] != ']') {
                    if ((unsigned char)*str >= (unsigned char)*pat &&
                        (unsigned char)*str <= (unsigned char)pat[2]) matched=1;
                    pat += 3;
                } else {
                    if (*str == *pat) matched = 1;
                    pat++;
                }
            }
            if (*pat == ']') pat++;
            if (matched == negate) return 0;
            str++;
            continue;
        }
        if (*pat == '\\' && pat[1]) { pat++; }
        if (*pat != *str) return 0;
        pat++; str++;
    }
}

static int str_cmp_wrap(const void *a, const void *b) {
    return strcmp(*(char**)a, *(char**)b);
}

char **glob_expand(shell_t *sh, const char *pat, int *outn) {
    const char *last_slash = strrchr(pat, '/');
    char *dir_part, *file_part;
    if (last_slash) {
        dir_part  = sh_strndup(pat, last_slash - pat + 1);
        file_part = sh_strdup(last_slash + 1);
    } else {
        dir_part  = sh_strdup(".");
        file_part = sh_strdup(pat);
    }

    int has_glob = 0;
    for (const char *p = file_part; *p; p++)
        if (*p == '*' || *p == '?' || *p == '[') { has_glob = 1; break; }

    if (!has_glob) {
        free(dir_part); free(file_part);
        char **res = sh_malloc(2 * sizeof(char*));
        res[0] = sh_strdup(pat); res[1] = NULL;
        *outn = 1;
        return res;
    }

    DIR *d = opendir(last_slash ? dir_part : ".");
    if (!d) {
        free(dir_part); free(file_part);
        char **res = sh_malloc(2 * sizeof(char*));
        res[0] = sh_strdup(pat); res[1] = NULL;
        *outn = 1;
        return res;
    }

    int cap = 16, n = 0;
    char **matches = sh_malloc(cap * sizeof(char*));
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.' && file_part[0] != '.') continue;
        if (pattern_match(file_part, de->d_name)) {
            if (n >= cap-1) { cap*=2; matches=sh_realloc(matches, cap*sizeof(char*)); }
            if (last_slash)
                matches[n++] = sh_asprintf("%s%s", dir_part, de->d_name);
            else
                matches[n++] = sh_strdup(de->d_name);
        }
    }
    closedir(d);
    free(dir_part); free(file_part);

    if (n == 0) {
        free(matches);
        matches = sh_malloc(2 * sizeof(char*));
        matches[0] = sh_strdup(pat); n = 1;
    } else {
        qsort(matches, n, sizeof(char*), str_cmp_wrap);
    }
    matches[n] = NULL;
    *outn = n;
    return matches;
}

typedef struct { const char *s; shell_t *sh; } AExpr;

static void ae_skip(AExpr *e) { while (isspace((unsigned char)*e->s)) e->s++; }
static long ae_expr(AExpr *e);

static long ae_primary(AExpr *e) {
    ae_skip(e);
    long v = 0;
    if (*e->s == '(') {
        e->s++;
        v = ae_expr(e);
        ae_skip(e);
        if (*e->s == ')') e->s++;
        return v;
    }
    if (*e->s == '-') { e->s++; return -ae_primary(e); }
    if (*e->s == '+') { e->s++; return  ae_primary(e); }
    if (*e->s == '!') { e->s++; return !ae_primary(e); }
    if (*e->s == '~') { e->s++; return ~ae_primary(e); }
    if (isdigit((unsigned char)*e->s) || *e->s=='-') {
        char *end;
        if (e->s[0]=='0' && (e->s[1]=='x'||e->s[1]=='X'))
            v = strtol(e->s, &end, 16);
        else if (e->s[0]=='0' && isdigit((unsigned char)e->s[1]))
            v = strtol(e->s, &end, 8);
        else
            v = strtol(e->s, &end, 10);
        e->s = end;
        return v;
    }
    if (isalpha((unsigned char)*e->s) || *e->s == '_') {
        char name[256]; int ni=0;
        while (isalnum((unsigned char)*e->s)||*e->s=='_') name[ni++]=*e->s++;
        name[ni]='\0';
        char *val = var_get(e->sh, name);
        if (val) v = strtol(val, NULL, 10);
        ae_skip(e);
        if (*e->s == '=') {
            e->s++;
            long rv = ae_expr(e);
            var_set(e->sh, name, sh_asprintf("%ld", rv), -1);
            return rv;
        }
        return v;
    }
    return 0;
}

static long ae_mul(AExpr *e) {
    long l = ae_primary(e);
    ae_skip(e);
    while (*e->s == '*' || *e->s == '/' || *e->s == '%') {
        char op = *e->s++; long r = ae_primary(e);
        if (op=='*') l*=r;
        else if (op=='/') { if (r) l/=r; else { sh_error(e->sh,"division by zero"); l=0; } }
        else { if (r) l%=r; else { sh_error(e->sh,"modulo by zero"); l=0; } }
        ae_skip(e);
    }
    return l;
}
static long ae_add(AExpr *e) {
    long l = ae_mul(e); ae_skip(e);
    while (*e->s == '+' || (*e->s=='-' && e->s[1]!='=')) {
        char op=*e->s++; long r=ae_mul(e); l = op=='+'?l+r:l-r; ae_skip(e);
    }
    return l;
}
static long ae_shift(AExpr *e) {
    long l=ae_add(e); ae_skip(e);
    while ((e->s[0]=='<'&&e->s[1]=='<')||(e->s[0]=='>'&&e->s[1]=='>')) {
        char op=*e->s; e->s+=2; long r=ae_add(e);
        l = op=='<'? l<<r : l>>r; ae_skip(e);
    }
    return l;
}
static long ae_cmp(AExpr *e) {
    long l=ae_shift(e); ae_skip(e);
    while ((*e->s=='<'||*e->s=='>')&&e->s[1]!='<'&&e->s[1]!='>') {
        char op=*e->s, eq=e->s[1]=='='; e->s+=1+eq; long r=ae_shift(e);
        if (op=='<') l = eq?l<=r:l<r; else l = eq?l>=r:l>r; ae_skip(e);
    }
    return l;
}
static long ae_eq(AExpr *e) {
    long l=ae_cmp(e); ae_skip(e);
    while ((e->s[0]=='='&&e->s[1]=='=')||(e->s[0]=='!'&&e->s[1]=='=')) {
        int eq=(e->s[0]=='='); e->s+=2; long r=ae_cmp(e);
        l = eq?l==r:l!=r; ae_skip(e);
    }
    return l;
}
static long ae_bitand(AExpr *e) { long l=ae_eq(e); ae_skip(e); while(*e->s=='&'&&e->s[1]!='&'){e->s++;l&=ae_eq(e);ae_skip(e);} return l; }
static long ae_bitxor(AExpr *e) { long l=ae_bitand(e); ae_skip(e); while(*e->s=='^'){e->s++;l^=ae_bitand(e);ae_skip(e);} return l; }
static long ae_bitor(AExpr *e)  { long l=ae_bitxor(e); ae_skip(e); while(*e->s=='|'&&e->s[1]!='|'){e->s++;l|=ae_bitxor(e);ae_skip(e);} return l; }
static long ae_land(AExpr *e)   { long l=ae_bitor(e); ae_skip(e); while(e->s[0]=='&'&&e->s[1]=='&'){e->s+=2;long r=ae_bitor(e);l=l&&r;ae_skip(e);} return l; }
static long ae_lor(AExpr *e)    { long l=ae_land(e); ae_skip(e); while(e->s[0]=='|'&&e->s[1]=='|'){e->s+=2;long r=ae_land(e);l=l||r;ae_skip(e);} return l; }
static long ae_ternary(AExpr *e) {
    long c=ae_lor(e); ae_skip(e);
    if (*e->s=='?') { e->s++; long t=ae_ternary(e); ae_skip(e);
        if(*e->s==':') e->s++;
        long f=ae_ternary(e); return c?t:f; }
    return c;
}
static long ae_expr(AExpr *e) { return ae_ternary(e); }

static int arith_eval(shell_t *sh, const char *expr, long *result) {
    char *expanded = expand_word(sh, expr);
    AExpr e; e.s = expanded; e.sh = sh;
    *result = ae_expr(&e);
    free(expanded);
    return 0;
}

static char *cmd_subst(shell_t *sh, const char *cmd) {
    int pfd[2];
    if (pipe(pfd) < 0) return sh_strdup("");
    pid_t pid = fork();
    if (pid == 0) {
        close(pfd[0]);
        dup2(pfd[1], 1);
        close(pfd[1]);
        shell_t *sub = sh_malloc(sizeof *sub);
        memcpy(sub, sh, sizeof *sub);
        sub->subshell = 1;
        sub->input = NULL;
        node_t *n = parse_string(sub, cmd);
        int st = n ? exec_node(sub, n) : 0;
        node_free(n);
        exit(st);
    }
    close(pfd[1]);
    size_t cap = 256, len = 0;
    char *buf = sh_malloc(cap);
    ssize_t nr;
    while ((nr = read(pfd[0], buf+len, cap-len-1)) > 0) {
        len += nr;
        if (len + 1 >= cap) { cap *= 2; buf = sh_realloc(buf, cap); }
    }
    buf[len] = '\0';
    close(pfd[0]);
    int status;
    waitpid(pid, &status, 0);
    sh->last_status = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    while (len > 0 && buf[len-1] == '\n') buf[--len] = '\0';
    return buf;
}

typedef struct { char *s; size_t n; size_t cap; } SB;
static void sb_init(SB *b) { b->s=sh_malloc(64); b->n=0; b->cap=64; b->s[0]='\0'; }
static void sb_push(SB *b, char c) {
    if (b->n+2>b->cap){b->cap*=2;b->s=sh_realloc(b->s,b->cap);}
    b->s[b->n++]=c; b->s[b->n]='\0';
}
static void sb_str(SB *b, const char *s) { if(s) while(*s) sb_push(b,*s++); }

static char *expand_brace(shell_t *sh, const char *inner) {
    const char *p = inner;

    if (*p == '#' && p[1] && p[1] != '}' && p[1] != '-' && p[1] != '=' && p[1] != '?' && p[1] != '+') {
        p++;
        char name[256]; int ni=0;
        while (*p && *p != '}') name[ni++] = *p++;
        name[ni] = '\0';
        if (strcmp(name,"@")==0 || strcmp(name,"*")==0) {
            char *np = var_get(sh,"#"); return np ? sh_strdup(np) : sh_strdup("0");
        }
        char *val = var_get(sh, name);
        if (!val) val = var_special(sh, name);
        return sh_asprintf("%zu", val ? strlen(val) : (size_t)0);
    }

    char name[256]; int ni = 0;
    if (*p == '@' || *p == '*' || *p == '?' || *p == '$' ||
        *p == '!' || *p == '#' || *p == '-' || *p == '0') {
        name[ni++] = *p++;
    } else {
        while (*p && (isalnum((unsigned char)*p) || *p == '_')) name[ni++] = *p++;
    }
    name[ni] = '\0';

    if (!*p) {
        char *val = var_get(sh, name);
        if (!val) val = var_special(sh, name);
        if (!val && sh->opt_u && is_name(name)) {
            sh_error(sh, "%s: unbound variable", name); longjmp(err_jmp, 1);
        }
        return sh_strdup(val ? val : "");
    }

    int colon_flag = 0;
    if (*p == ':') { colon_flag = 1; p++; }
    char op = *p ? *p++ : '\0';
    const char *word_str = p;

    char *val = var_get(sh, name);
    if (!val) val = var_special(sh, name);

    int is_unset = (val == NULL);
    int is_null  = (val != NULL && *val == '\0');
    int trigger  = colon_flag ? (is_unset || is_null) : is_unset;

    switch (op) {
    case '-': {
        if (trigger) { char *w = expand_word(sh, word_str); return w; }
        return sh_strdup(val ? val : "");
    }
    case '=': {
        if (trigger) {
            char *w = expand_word(sh, word_str);
            if (is_name(name)) var_set(sh, name, w, -1);
            return w;
        }
        return sh_strdup(val ? val : "");
    }
    case '+': {
        if (!trigger) { char *w = expand_word(sh, word_str); return w; }
        return sh_strdup("");
    }
    case '?': {
        if (trigger) {
            char *w = expand_word(sh, word_str);
            sh_error(sh, "%s: %s", name, *w ? w : "parameter null or not set");
            free(w); longjmp(err_jmp, 1);
        }
        return sh_strdup(val ? val : "");
    }
    case '#': {
        int greedy = (*p == '#');
        if (greedy) p++;
        char *pat = expand_word(sh, word_str);
        char *s   = sh_strdup(val ? val : "");
        size_t slen = strlen(s);
        char *result = s;
        if (!greedy) {
            for (size_t i = 0; i <= slen; i++) {
                char saved_c = s[i]; s[i] = '\0';
                int m = pattern_match(pat, s);
                s[i] = saved_c;
                if (m) { result = sh_strdup(s + i); free(s); free(pat); return result; }
            }
        } else {
            for (size_t i = slen; ; i--) {
                char saved_c = s[i]; s[i] = '\0';
                int m = pattern_match(pat, s);
                s[i] = saved_c;
                if (m) { result = sh_strdup(s + i); free(s); free(pat); return result; }
                if (i == 0) break;
            }
        }
        free(pat);
        return s;
    }
    case '%': {
        int greedy = (*p == '%');
        if (greedy) p++;
        char *pat = expand_word(sh, word_str);
        char *s   = sh_strdup(val ? val : "");
        size_t slen = strlen(s);
        if (!greedy) {
            for (size_t i = slen; ; i--) {
                if (pattern_match(pat, s + i)) {
                    char *r = sh_strndup(s, i); free(s); free(pat); return r;
                }
                if (i == 0) break;
            }
        } else {
            for (size_t i = 0; i <= slen; i++) {
                if (pattern_match(pat, s + i)) {
                    char *r = sh_strndup(s, i); free(s); free(pat); return r;
                }
            }
        }
        free(pat);
        return s;
    }
    default:
        return sh_strdup(val ? val : "");
    }
}

static char *get_positional(shell_t *sh, int n) {
    char name[32];
    snprintf(name, sizeof name, "%d", n);
    return var_get(sh, name);
}

static int get_nparams(shell_t *sh) {
    char *v = var_get(sh, "#");
    return v ? atoi(v) : 0;
}

static char *expand_one(shell_t *sh, const char *s, int in_dquote) {
    SB out; sb_init(&out);
    const char *p = s;

    if (*p == '~') {
        p++;
        const char *nstart = p;
        while (*p && *p != '/' && *p != ':') p++;
        size_t nlen = p - nstart;
        if (nlen == 0) {
            char *home = var_get(sh, "HOME");
            sb_str(&out, home ? home : "/");
        } else {
            char uname[256];
            if (nlen < sizeof uname) {
                memcpy(uname, nstart, nlen); uname[nlen] = '\0';
                struct passwd *pw = getpwnam(uname);
                if (pw) sb_str(&out, pw->pw_dir);
                else { sb_push(&out, '~'); for (size_t i=0;i<nlen;i++) sb_push(&out,nstart[i]); }
            } else {
                sb_push(&out, '~');
                for (size_t i=0;i<nlen;i++) sb_push(&out, nstart[i]);
            }
        }
    }

    while (*p) {
        if ((unsigned char)*p == 0x01) {
            p++;
            while (*p && (unsigned char)*p != 0x01) sb_push(&out, *p++);
            if ((unsigned char)*p == 0x01) p++;
            continue;
        }
        if (*p == '\\') {
            p++;
            if (*p) sb_push(&out, *p++);
            continue;
        }
        if (*p == '"') {
            p++;
            while (*p && *p != '"') {
                if ((unsigned char)*p == 0x01) {
                    p++;
                    while (*p && (unsigned char)*p != 0x01) sb_push(&out, *p++);
                    if ((unsigned char)*p == 0x01) p++;
                } else if (*p == '\\') {
                    p++;
                    if (*p == '$'||*p=='"'||*p=='`'||*p=='\\') sb_push(&out, *p++);
                    else { sb_push(&out,'\\'); if(*p) sb_push(&out,*p++); }
                } else if (*p == '$') {
                    p++;
                    char *sub = NULL;
                    if (*p == '(') {
                        if (p[1] == '(') {
                            p += 2;
                            const char *start = p;
                            int depth = 2;
                            while (*p) { if(*p=='(')depth++; else if(*p==')'){depth--;if(depth==0){p++;break;}} p++; }
                            if (*p == ')') p++;
                            char *expr = sh_strndup(start, p - start - 2);
                            long result; arith_eval(sh, expr, &result); free(expr);
                            sub = sh_asprintf("%ld", result);
                        } else {
                            p++;
                            const char *start = p;
                            int depth = 1;
                            while (*p) {
                                if (*p=='\\' && p[1]) { p+=2; continue; }
                                if (*p=='\'') { p++; while(*p&&*p!='\'') p++; if(*p) p++; continue; }
                                if (*p=='"') { p++; while(*p&&*p!='"'){if(*p=='\\'&&p[1])p++;p++;} if(*p)p++; continue; }
                                if(*p=='(')depth++; else if(*p==')'){depth--;if(depth==0){p++;break;}}
                                p++;
                            }
                            char *cmd = sh_strndup(start, p - start - 1);
                            sub = cmd_subst(sh, cmd); free(cmd);
                        }
                    } else if (*p == '{') {
                        p++;
                        const char *start = p;
                        int depth = 1;
                        while (*p) { if(*p=='{')depth++; else if(*p=='}'){depth--;if(depth==0){p++;break;}} p++; }
                        char *inner = sh_strndup(start, p - start - 1);
                        sub = expand_brace(sh, inner); free(inner);
                    } else if (*p == '$') { p++; sub = sh_asprintf("%d",(int)getpid()); }
                    else if (*p == '?') { p++; sub = sh_asprintf("%d",sh->last_status); }
                    else if (*p == '!') { p++; sub = var_special(sh,"!"); if(!sub)sub=sh_strdup(""); }
                    else if (*p == '#') { p++; sub = sh_asprintf("%d", get_nparams(sh)); }
                    else if (*p == '-') { p++; sub = var_special(sh,"-"); if(!sub)sub=sh_strdup(""); }
                    else if (*p == '@' || *p == '*') {
                        int is_at = (*p == '@');
                        p++;
                        int n = get_nparams(sh);
                        SB ps; sb_init(&ps);
                        for (int i=1;i<=n;i++) {
                            char *pv = get_positional(sh,i);
                            if (i > 1) {
                                if (is_at)
                                    sb_push(&ps, '\x03');
                                else {
                                    char sep = sh->ifs && sh->ifs[0] ? sh->ifs[0] : ' ';
                                    sb_push(&ps, sep);
                                }
                            }
                            if (pv) sb_str(&ps, pv);
                        }
                        sub = ps.s;
                    } else if (isdigit((unsigned char)*p)) {
                        char *end; long idx = strtol(p, &end, 10); p = end;
                        if (idx == 0) sub = sh_strdup(sh->script_name ? sh->script_name : "aurum");
                        else { char *pv = get_positional(sh,(int)idx); sub = pv ? sh_strdup(pv) : sh_strdup(""); }
                    } else if (isalpha((unsigned char)*p)||*p=='_') {
                        char name[256]; int ni=0;
                        while (isalnum((unsigned char)*p)||*p=='_') name[ni++]=*p++;
                        name[ni]='\0';
                        char *v = var_get(sh, name);
                        if (!v && sh->opt_u) { sh_error(sh,"%s: unbound variable",name); longjmp(err_jmp,1); }
                        sub = sh_strdup(v ? v : "");
                    } else { sb_push(&out,'$'); continue; }
                    if (sub) { sb_str(&out, sub); free(sub); }
                    continue;
                } else if (*p == '`') {
                    p++;
                    const char *start = p;
                    while (*p && *p != '`') { if(*p=='\\')p++; p++; }
                    char *cmd = sh_strndup(start, p - start);
                    if (*p == '`') p++;
                    char *sub2 = cmd_subst(sh, cmd); free(cmd);
                    sb_str(&out, sub2); free(sub2);
                } else {
                    sb_push(&out, *p++);
                }
            }
            if (*p == '"') p++;
            continue;
        }
        if (*p == '$') {
            p++;
            if (*p == '(') {
                if (p[1] == '(') {
                    p += 2;
                    const char *start = p; int depth=2;
                    while(*p){if(*p=='(')depth++;else if(*p==')'){depth--;if(depth==0){p++;break;}}p++;}
                    if(*p==')') p++;
                    char *expr = sh_strndup(start, p-start-2);
                    long result; arith_eval(sh,expr,&result); free(expr);
                    char *sub = sh_asprintf("%ld",result);
                    sb_str(&out, sub); free(sub);
                } else {
                    p++;
                    const char *start=p; int depth=1;
                    while(*p) {
                        if (*p == '\\' && p[1]) { p+=2; continue; }
                        if (*p == '\'') { p++; while(*p && *p!='\'') p++; if(*p) p++; continue; }
                        if (*p == '"') {
                            p++;
                            while (*p && *p!='"') {
                                if (*p=='\\' && p[1]) p++;
                                p++;
                            }
                            if (*p) p++;
                            continue;
                        }
                        if (*p=='(') depth++;
                        else if (*p==')') { depth--; if(depth==0){p++;break;} }
                        p++;
                    }
                    char *cmd = sh_strndup(start, p-start-1);
                    char *sub = cmd_subst(sh,cmd); free(cmd);
                    sb_str(&out,sub); free(sub);
                }
            } else if (*p == '{') {
                p++;
                const char *start=p; int depth=1;
                while(*p){if(*p=='{')depth++;else if(*p=='}'){depth--;if(depth==0){p++;break;}}p++;}
                char *inner = sh_strndup(start, p-start-1);
                char *sub = expand_brace(sh, inner); free(inner);
                sb_str(&out, sub); free(sub);
            } else if (*p == '$') { p++; sb_str(&out, sh_asprintf("%d",(int)getpid())); }
            else if (*p == '?') { p++; sb_str(&out, sh_asprintf("%d",sh->last_status)); }
            else if (*p == '!') { p++; char *v=var_special(sh,"!"); sb_str(&out,v?v:""); free(v); }
            else if (*p == '#') { p++; sb_str(&out, sh_asprintf("%d",get_nparams(sh))); }
            else if (*p == '-') { p++; char *v=var_special(sh,"-"); sb_str(&out,v?v:""); free(v); }
            else if (*p == '@' || *p == '*') {
                p++;
                int n = get_nparams(sh);
                for (int i=1;i<=n;i++) {
                    if(i>1) sb_push(&out,' ');
                    char *pv = get_positional(sh,i);
                    if(pv) sb_str(&out,pv);
                }
            } else if (isdigit((unsigned char)*p)) {
                char *end; long idx=strtol(p,&end,10); p=end;
                if(idx==0) sb_str(&out, sh->script_name?sh->script_name:"aurum");
                else { char *pv=get_positional(sh,(int)idx); sb_str(&out,pv?pv:""); }
            } else if (isalpha((unsigned char)*p)||*p=='_') {
                char name[256]; int ni=0;
                while(isalnum((unsigned char)*p)||*p=='_') name[ni++]=*p++;
                name[ni]='\0';
                char *v = var_get(sh,name);
                if(!v) v = var_special(sh, name);
                if(!v && sh->opt_u) { sh_error(sh,"%s: unbound variable",name); longjmp(err_jmp,1); }
                sb_str(&out, v?v:"");
            } else { sb_push(&out,'$'); continue; }
            continue;
        }
        if (*p == '`') {
            p++;
            const char *start=p;
            while(*p && *p!='`') { if(*p=='\\')p++; p++; }
            char *cmd = sh_strndup(start, p-start);
            if(*p=='`') p++;
            char *sub = cmd_subst(sh,cmd); free(cmd);
            sb_str(&out,sub); free(sub);
            continue;
        }
        sb_push(&out, *p);
        if (*p == '=' && p[1] == '~') {
            p += 2;
            const char *nstart = p;
            while (*p && *p != '/' && *p != ':') p++;
            size_t nlen = p - nstart;
            if (nlen == 0) {
                char *home = var_get(sh, "HOME");
                sb_str(&out, home ? home : "/");
            } else {
                char uname[256];
                if (nlen < sizeof uname) {
                    memcpy(uname, nstart, nlen); uname[nlen] = '\0';
                    struct passwd *pw = getpwnam(uname);
                    if (pw) sb_str(&out, pw->pw_dir);
                    else { sb_push(&out,'~'); for(size_t i=0;i<nlen;i++) sb_push(&out,nstart[i]); }
                } else {
                    sb_push(&out,'~');
                    for(size_t i=0;i<nlen;i++) sb_push(&out,nstart[i]);
                }
            }
        } else p++;
    }
    return out.s;
}

char *expand_word(shell_t *sh, const char *w) {
    return expand_one(sh, w, 0);
}

static char **ifs_split(shell_t *sh, const char *s, int *outn) {
    const char *ifs = sh->ifs ? sh->ifs : " \t\n";
    int cap=8, n=0;
    char **res = sh_malloc(cap*sizeof(char*));

    const char *p = s;
    while (*p) {
        if ((unsigned char)*p == 0x03) { p++; continue; }
        while (*p && (unsigned char)*p != 0x03 && strchr(ifs, *p) && isspace((unsigned char)*p)) p++;
        if (!*p || (unsigned char)*p == 0x03) { if ((unsigned char)*p == 0x03) p++; continue; }
        const char *start = p;
        while (*p && (unsigned char)*p != 0x03 && !strchr(ifs, *p)) p++;
        if (p > start) {
            if (n >= cap-1) { cap*=2; res=sh_realloc(res,cap*sizeof(char*)); }
            res[n++] = sh_strndup(start, p-start);
        }
        if (*p && (unsigned char)*p != 0x03 && strchr(ifs, *p) && !isspace((unsigned char)*p)) p++;
        if ((unsigned char)*p == 0x03) p++;
    }
    if (n == 0) {
        res[0] = NULL;
        *outn = 0;
        return res;
    }
    res[n] = NULL;
    *outn = n;
    return res;
}

char **expand_words(shell_t *sh, char **words, int nwords, int *outc) {
    int cap=16, n=0;
    char **res = sh_malloc(cap*sizeof(char*));

    for (int i=0; i<nwords; i++) {
        const char *w = words[i];
        char *expanded = expand_one(sh, w, 0);

        int was_quoted = 0;
        for (const char *c = w; *c; c++) {
            if (*c == '"' || *c == '\\' || (unsigned char)*c == 0x01) { was_quoted=1; break; }
        }

        int has_sentinel = 0;
        for (const char *c = expanded; *c; c++)
            if ((unsigned char)*c == 0x03) { has_sentinel=1; break; }

        if (was_quoted && !has_sentinel) {
            if (n >= cap-1) { cap*=2; res=sh_realloc(res,cap*sizeof(char*)); }
            res[n++] = expanded;
        } else if (was_quoted && has_sentinel) {
            char *p = expanded;
            while (1) {
                char *sep = p;
                while (*sep && (unsigned char)*sep != 0x03) sep++;
                if (sep > p) {
                    if (n >= cap-1) { cap*=2; res=sh_realloc(res,cap*sizeof(char*)); }
                    res[n++] = sh_strndup(p, sep-p);
                }
                if (!*sep) break;
                p = sep+1;
            }
            free(expanded);
        } else {
            int nsplit=0;
            char **fields = ifs_split(sh, expanded, &nsplit);
            free(expanded);
            for (int j=0; j<nsplit; j++) {
                if (!sh->opt_f) {
                    int ng=0;
                    char **globs = glob_expand(sh, fields[j], &ng);
                    free(fields[j]);
                    for (int k=0; k<ng; k++) {
                        if (n >= cap-1) { cap*=2; res=sh_realloc(res,cap*sizeof(char*)); }
                        res[n++] = globs[k];
                    }
                    free(globs);
                } else {
                    if (n >= cap-1) { cap*=2; res=sh_realloc(res,cap*sizeof(char*)); }
                    res[n++] = fields[j];
                }
            }
            free(fields);
        }
    }
    res[n] = NULL;
    *outc = n;
    return res;
}

char *expand_param(shell_t *sh, const char *w) {
    return expand_one(sh, w, 0);
}