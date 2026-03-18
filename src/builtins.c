#include "aurum.h"

#include "parser.h"
#include "string.h"
#include "builtins.h"
#include "util.h"
#include "ctype.h"
#include "stdlib.h"
#include "vars.h"
#include "unistd.h"
#include "exec.h"
#include "limits.h"
#include "errno.h"
#include "jobs.h"
#include "sys/wait.h"
#include "sys/stat.h"

static const char *BUILTINS[] = {
    ":",
    ".",
    "source",
    "break",
    "continue",
    "return",
    "exit",
    "cd",
    "pwd",
    "echo",
    "printf",
    "read",
    "shift",
    "set",
    "unset",
    "export",
    "readonly",
    "local",
    "eval",
    "exec",
    "wait",
    "jobs",
    "true",
    "false",
    "test",
    "[",
    "type",
    "command",
    "getopts",
    "umask",
    "ulimit",
    "times",
    "hash",
#ifdef USE_JOBCONTROL
    "trap",
    "fg",
    "bg",
    "kill",
#endif
    NULL
};

int is_builtin(const char *name) {
    for (int i = 0; BUILTINS[i]; i++) {
        if (strcmp(BUILTINS[i], name) == 0) {
            return 1;
        }
    }

    return 0;
}

static char *trap_actions[32];

void trap_run(shell_t *sh, int sig) {
    if (sig < 0 || sig >= 32 || !trap_actions[sig]) {
        return;
    }

    node_t *n = parse_string(sh, trap_actions[sig]);

    if (n) {
        exec_node(sh, n);
        node_free(n);
    }
}

static int bi_colon(shell_t *sh, char **a, int n) {
    (void)sh;
    (void)a;
    (void)n;
    return 0;
}

static int bi_true (shell_t *sh, char **a, int n) {
    (void)sh;
    (void)a;
    (void)n;
    return 0;
}

static int bi_false(shell_t *sh, char **a, int n) {
    (void)sh;
    (void)a;
    (void)n;
    return 1;
}

static int bi_echo(shell_t *sh, char **argv, int argc) {
    (void)sh;
    int no_newline = 0;
    int interpret = 0;
    int i = 1;

    for (; i < argc && argv[i][0] == '-'; i++) {
        char *p = argv[i] + 1;
        if (!*p) {
            break;
        }

        int ok = 1;
        for (; *p; p++) {
            if      (*p == 'n') no_newline = 1;
            else if (*p == 'e') interpret  = 1;
            else if (*p == 'E') interpret  = 0;
            else {
                ok = 0;
                break;
            }
        }
        if (!ok) {
            break;
        }
    }
    
    for (int j = i; j < argc; j++) {
        if (j > i) putchar(' ');
        if (interpret) {
            for (char *p = argv[j]; *p; p++) {
                if (*p == '\\' && p[1]) {
                    p++;
                    switch (*p) {
                    case 'n': putchar('\n'); break;
                    case 't': putchar('\t'); break;
                    case 'r': putchar('\r'); break;
                    case '\\': putchar('\\'); break;
                    case 'c': no_newline = 1; goto echo_done;
                    case 'a': putchar('\a'); break;
                    case 'b': putchar('\b'); break;
                    case 'f': putchar('\f'); break;
                    case 'v': putchar('\v'); break;
                    case '0': {
                        int v = 0, k = 0;
                        while (k < 3 && p[1] >= '0' && p[1] <= '7')
                            { p++; v = v*8 + (*p-'0'); k++; }
                        putchar(v); break;
                    }
                    default: putchar('\\'); putchar(*p);
                    }
                } else putchar(*p);
            }
        } else fputs(argv[j], stdout);
    }
echo_done:
    if (!no_newline) putchar('\n');
    fflush(stdout);
    return 0;
}

static int bi_printf(shell_t *sh, char **argv, int argc) {
    if (argc < 2) { sh_error(sh,"printf: usage: printf format [arg...]"); return 1; }
    const char *fmt = argv[1];
    int ai = 2;
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            if (*p == '\\') {
                p++;
                switch (*p) {
                case 'n': putchar('\n'); break; case 't': putchar('\t'); break;
                case '\\': putchar('\\'); break; case 'a': putchar('\a'); break;
                case 'b': putchar('\b'); break; case 'r': putchar('\r'); break;
                case 'v': putchar('\v'); break; case 'f': putchar('\f'); break;
                default: putchar('\\'); if (*p) putchar(*p); break;
                }
            } else putchar(*p);
            continue;
        }
        p++;
        char spec[32]; int si = 0;
        while (*p=='-'||*p=='+'||*p==' '||*p=='0'||*p=='#') spec[si++]=*p++;
        while (isdigit((unsigned char)*p)) spec[si++]=*p++;
        if (*p=='.') { spec[si++]=*p++; while(isdigit((unsigned char)*p)) spec[si++]=*p++; }
        char conv = *p ? *p++ : 's';
        spec[si++]=conv; spec[si]='\0';
        char rfmt[64]; snprintf(rfmt,sizeof rfmt,"%%%s",spec);
        char *arg = ai < argc ? argv[ai++] : "";
        switch (conv) {
        case 'd': case 'i': printf(rfmt,(int)strtol(arg,NULL,10)); break;
        case 'u': printf(rfmt,(unsigned)strtoul(arg,NULL,10)); break;
        case 'o': printf(rfmt,(unsigned)strtoul(arg,NULL,10)); break;
        case 'x': case 'X': printf(rfmt,(unsigned)strtoul(arg,NULL,10)); break;
        case 'f': case 'e': case 'E': case 'g': case 'G':
                  printf(rfmt,strtod(arg,NULL)); break;
        case 'c': printf(rfmt,*arg); break;
        case 's': printf(rfmt,arg); break;
        case '%': putchar('%'); break;
        default:  fputs(arg,stdout);
        }
    }
    fflush(stdout);
    return 0;
}

static int bi_read(shell_t *sh, char **argv, int argc) {
    char *prompt = NULL;
    int raw = 0, i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i],"-r")) raw = 1;
        else if (!strcmp(argv[i],"-p") && i+1<argc) prompt = argv[++i];
        else break;
    }
    if (prompt && sh->interactive) { fputs(prompt,stderr); fflush(stderr); }
    char *line = NULL; size_t lsz = 0;
    ssize_t ll = getline(&line, &lsz, stdin);
    if (ll < 0) { free(line); return 1; }
    if (ll > 0 && line[ll-1]=='\n') { line[ll-1]='\0'; ll--; }
    if (!raw) {
        while (ll > 0 && line[ll-1]=='\\') {
            line[ll-1]='\0'; ll--;
            char *more=NULL; size_t ms=0;
            ssize_t ml = getline(&more,&ms,stdin);
            if (ml<0) { free(more); break; }
            if (ml>0&&more[ml-1]=='\n') { more[ml-1]='\0'; ml--; }
            char *nl = sh_asprintf("%s%s",line,more);
            free(line); free(more);
            line=nl; ll=strlen(line); lsz=ll+1;
        }
    }
    int nnames = argc - i;
    if (nnames == 0) { var_set(sh,"REPLY",line,-1); free(line); return 0; }
    const char *ifs = sh->ifs ? sh->ifs : " \t\n";
    char *p = line;
    for (int j = i; j < argc; j++) {
        while (*p && strchr(ifs,*p) && isspace((unsigned char)*p)) p++;
        if (j == argc-1) {
            var_set(sh,argv[j],p,-1); p += strlen(p);
        } else {
            char *start=p;
            while (*p && !strchr(ifs,*p)) p++;
            char *val=sh_strndup(start,p-start);
            var_set(sh,argv[j],val,-1); free(val);
            if (*p) p++;
        }
    }
    free(line);
    return 0;
}

static int bi_cd(shell_t *sh, char **argv, int argc) {
    const char *dir = NULL;
    if (argc == 1) {
        dir = var_get(sh,"HOME");
        if (!dir) { sh_error(sh,"cd: HOME not set"); return 1; }
    } else if (argc == 2) {
        if (!strcmp(argv[1],"-")) {
            dir = var_get(sh,"OLDPWD");
            if (!dir) { sh_error(sh,"cd: OLDPWD not set"); return 1; }
            printf("%s\n",dir);
        } else dir = argv[1];
    } else { sh_error(sh,"cd: too many arguments"); return 1; }
    char cwd[PATH_MAX];
    if (!getcwd(cwd,sizeof cwd)) cwd[0]='\0';
    if (chdir(dir) < 0) { sh_error(sh,"cd: %s: %s",dir,strerror(errno)); return 1; }
    var_set(sh,"OLDPWD",cwd,-1);
    if (getcwd(cwd,sizeof cwd)) var_set(sh,"PWD",cwd,-1);
    return 0;
}

static int bi_pwd(shell_t *sh, char **argv, int argc) {
    (void)argv; (void)argc;
    char cwd[PATH_MAX];
    if (!getcwd(cwd,sizeof cwd)) { sh_error(sh,"pwd: %s",strerror(errno)); return 1; }
    puts(cwd);
    return 0;
}

static int bi_exit(shell_t *sh, char **argv, int argc) {
    int code = sh->last_status;
    if (argc > 1) code = atoi(argv[1]);
    sh->do_exit    = 1;
    sh->exit_status = code & 0xFF;
    return code;
}

static int bi_return(shell_t *sh, char **argv, int argc) {
    int code = sh->last_status;
    if (argc > 1) code = atoi(argv[1]);
    sh->do_return = 1;
    sh->returning = code;
    return code;
}

static int bi_break(shell_t *sh, char **argv, int argc) {
    int n = argc > 1 ? atoi(argv[1]) : 1;
    if (n < 1) n = 1;
    sh->breaking = n;
    return 0;
}
static int bi_continue(shell_t *sh, char **argv, int argc) {
    int n = argc > 1 ? atoi(argv[1]) : 1;
    if (n < 1) n = 1;
    sh->continuing = n;
    return 0;
}

static int bi_shift(shell_t *sh, char **argv, int argc) {
    int n = argc > 1 ? atoi(argv[1]) : 1;
    char *np = var_get(sh,"#");
    int total = np ? atoi(np) : 0;
    if (n > total) { sh_error(sh,"shift: shift count out of range"); return 1; }
    for (int i=1; i<=total-n; i++) {
        char from[32],to[32];
        snprintf(from,sizeof from,"%d",i+n);
        snprintf(to,  sizeof to,  "%d",i);
        char *v = var_get(sh,from);
        var_set(sh,to,v?v:"",-1);
    }
    for (int i=total-n+1; i<=total; i++) {
        char nm[32]; snprintf(nm,sizeof nm,"%d",i);
        var_unset(sh,nm);
    }
    var_set(sh,"#",sh_asprintf("%d",total-n),-1);
    return 0;
}

static int bi_export(shell_t *sh, char **argv, int argc) {
    if (argc == 1) {
        for (var_t *v=sh->vars; v; v=v->next)
            if (v->exported) printf("export %s=%s\n",v->name,v->val?v->val:"");
        return 0;
    }
    for (int i=1; i<argc; i++) {
        char *eq = strchr(argv[i],'=');
        if (eq) {
            char *nm = sh_strndup(argv[i],eq-argv[i]);
            var_set(sh,nm,eq+1,1); free(nm);
        } else {
            var_t *v = var_find(sh,argv[i]);
            if (v) v->exported=1; else var_set(sh,argv[i],"",1);
        }
    }
    return 0;
}

static int bi_readonly(shell_t *sh, char **argv, int argc) {
    if (argc == 1) {
        for (var_t *v=sh->vars; v; v=v->next)
            if (v->readonly) printf("readonly %s=%s\n",v->name,v->val?v->val:"");
        return 0;
    }
    for (int i=1; i<argc; i++) {
        char *eq = strchr(argv[i],'=');
        if (eq) {
            char *nm = sh_strndup(argv[i],eq-argv[i]);
            var_set(sh,nm,eq+1,-1);
            var_t *v=var_find(sh,nm); if(v) v->readonly=1; free(nm);
        } else {
            var_t *v=var_find(sh,argv[i]);
            if (!v) { var_set(sh,argv[i],"",-1); v=var_find(sh,argv[i]); }
            if (v) v->readonly=1;
        }
    }
    return 0;
}

static int bi_unset(shell_t *sh, char **argv, int argc) {
    int func=0, i=1;
    for (; i<argc && argv[i][0]=='-'; i++) {
        if (!strcmp(argv[i],"-f")) func=1;
        else if (!strcmp(argv[i],"-v")) {}
    }
    for (; i<argc; i++) {
        if (func) {
            for (func_t **fp=&sh->funcs; *fp; fp=&(*fp)->next) {
                if (!strcmp((*fp)->name,argv[i])) {
                    func_t *fn=*fp; *fp=fn->next;
                    free(fn->name); node_free(fn->body); free(fn); break;
                }
            }
        } else var_unset(sh,argv[i]);
    }
    return 0;
}

static int bi_set(shell_t *sh, char **argv, int argc) {
    if (argc == 1) {
        for (var_t *v=sh->vars; v; v=v->next) printf("%s=%s\n",v->name,v->val?v->val:"");
        return 0;
    }
    int i=1, end_opts=0;
    for (; i<argc && !end_opts; i++) {
        if (!strcmp(argv[i],"--")||!strcmp(argv[i],"-")) { end_opts=1; i++; break; }
        if (argv[i][0]!='-' && argv[i][0]!='+') { end_opts=1; break; }
        int on=(argv[i][0]=='-');
        for (char *p=argv[i]+1; *p; p++) {
            switch (*p) {
            case 'x': sh->opt_x=on; break; case 'e': sh->opt_e=on; break;
            case 'u': sh->opt_u=on; break; case 'n': sh->opt_n=on; break;
            case 'v': sh->opt_v=on; break; case 'f': sh->opt_f=on; break;
            case 'a': sh->opt_a=on; break; case 'm': sh->opt_m=on; break;
            case 'C': sh->opt_C=on; break;
            case 'o':
                if (i+1<argc) {
                    i++;
                    if      (!strcmp(argv[i],"xtrace"))    sh->opt_x=on;
                    else if (!strcmp(argv[i],"errexit"))   sh->opt_e=on;
                    else if (!strcmp(argv[i],"nounset"))   sh->opt_u=on;
                    else if (!strcmp(argv[i],"noexec"))    sh->opt_n=on;
                    else if (!strcmp(argv[i],"verbose"))   sh->opt_v=on;
                    else if (!strcmp(argv[i],"noglob"))    sh->opt_f=on;
                    else if (!strcmp(argv[i],"allexport")) sh->opt_a=on;
                    else if (!strcmp(argv[i],"monitor"))   sh->opt_m=on;
                    else if (!strcmp(argv[i],"noclobber")) sh->opt_C=on;
                    else { sh_error(sh,"set: unknown option: %s",argv[i]); return 1; }
                }
                break;
            default: sh_error(sh,"set: unknown flag: -%c",*p); return 1;
            }
        }
    }
    if (i <= argc-1 || end_opts) {
        int n = argc-i;
        char *old_np=var_get(sh,"#"); int old_n=old_np?atoi(old_np):0;
        for (int j=1;j<=old_n;j++) { char nm[32]; snprintf(nm,sizeof nm,"%d",j); var_unset(sh,nm); }
        for (int j=0;j<n;j++) { char nm[32]; snprintf(nm,sizeof nm,"%d",j+1); var_set(sh,nm,argv[i+j],-1); }
        var_set(sh,"#",sh_asprintf("%d",n),-1);
    }
    return 0;
}

static int bi_local(shell_t *sh, char **argv, int argc) {
    for (int i=1; i<argc; i++) {
        char *eq=strchr(argv[i],'=');
        char *name, *val;
        if (eq) { name=sh_strndup(argv[i],eq-argv[i]); val=eq+1; }
        else    { name=sh_strdup(argv[i]); val=""; }
        var_t *v=sh_malloc(sizeof *v);
        v->name=sh_strdup(name); v->val=sh_strdup(val);
        v->exported=0; v->readonly=0;
        v->next=sh->locals; sh->locals=v;
        free(name);
    }
    return 0;
}

static int bi_eval(shell_t *sh, char **argv, int argc) {
    if (argc <= 1) return 0;
    char *cmd = join_words(argv+1,argc-1," ");
    node_t *n = parse_string(sh,cmd); free(cmd);
    if (!n) return 0;
    int r = exec_node(sh,n); node_free(n);
    return r;
}

static int bi_exec(shell_t *sh, char **argv, int argc) {
    if (argc == 1) return 0;
    char **envp = build_envp(sh);
    execve(argv[1],argv+1,envp);
    sh_error(sh,"exec: %s: %s",argv[1],strerror(errno));
    free_envp(envp);
    return 126;
}

static int bi_wait(shell_t *sh, char **argv, int argc) {
    if (argc == 1) {
        int status;
        while (waitpid(-1,&status,0) > 0) {}
        return 0;
    }
    return job_wait(sh, (pid_t)atoi(argv[1]));
}

static int bi_jobs(shell_t *sh, char **argv, int argc) {
    (void)argv; (void)argc;
    jobs_print(sh);
    return 0;
}

static int bi_type(shell_t *sh, char **argv, int argc) {
    int ret=0;
    for (int i=1; i<argc; i++) {
        if (is_builtin(argv[i])) { printf("%s is a shell builtin\n",argv[i]); continue; }
        int ff=0;
        for (func_t *f=sh->funcs; f; f=f->next)
            if (!strcmp(f->name,argv[i])) { printf("%s is a function\n",argv[i]); ff=1; break; }
        if (ff) continue;
        char *path=var_get(sh,"PATH");
        if (!path) path="/bin:/usr/bin";
        char *pc=sh_strdup(path), *dir=pc, *found=NULL;
        while (dir&&*dir) {
            char *col=strchr(dir,':'); if(col) *col='\0';
            char full[PATH_MAX];
            snprintf(full,sizeof full,"%s/%s",*dir?dir:".",argv[i]);
            struct stat st;
            if (stat(full,&st)==0&&(st.st_mode&S_IXUSR)) { found=sh_strdup(full); break; }
            dir=col?col+1:NULL;
        }
        free(pc);
        if (found) { printf("%s is %s\n",argv[i],found); free(found); }
        else { printf("%s not found\n",argv[i]); ret=1; }
    }
    return ret;
}

static int bi_command(shell_t *sh, char **argv, int argc) {
    int i=1, verbose=0;
    for (; i<argc && argv[i][0]=='-'; i++) {
        if (!strcmp(argv[i],"-v")||!strcmp(argv[i],"-V")) verbose=1;
        else if (!strcmp(argv[i],"-p")) {}
        else break;
    }
    if (i >= argc) return 0;
    if (verbose) { char *fa[]={"type",argv[i]}; return bi_type(sh,fa,2); }
    return exec_external(sh,argv+i,argc-i);
}

static int bi_getopts(shell_t *sh, char **argv, int argc) {
    if (argc < 3) { sh_error(sh,"getopts: usage: getopts optstring name [arg...]"); return 2; }
    char *optstring=argv[1], *varname=argv[2];
    char *oi_str=var_get(sh,"OPTIND"); int optind=oi_str?atoi(oi_str):1;
    if (optind<1) optind=1;
    char **args=argv+3; int nargs=argc-3;
    int free_args=0;
    if (nargs==0) {
        char *np=var_get(sh,"#"); nargs=np?atoi(np):0;
        args=sh_malloc((nargs+1)*sizeof(char*));
        for (int j=1;j<=nargs;j++) {
            char nm[32]; snprintf(nm,sizeof nm,"%d",j);
            args[j-1]=var_get(sh,nm);
        }
        args[nargs]=NULL; free_args=1;
    }
    if (optind>nargs || !args[optind-1] || args[optind-1][0]!='-' ||
        !strcmp(args[optind-1],"-") || !strcmp(args[optind-1],"--")) {
        var_set(sh,varname,"?",-1);
        if (free_args) free(args);
        return 1;
    }
    char *arg=args[optind-1];
    char *oc_str=var_get(sh,"OPTIND_C"); int oc=oc_str?atoi(oc_str):1;
    char opt=arg[oc];
    char *op=strchr(optstring,opt);
    if (!op) { var_set(sh,varname,"?",-1); var_set(sh,"OPTARG","",-1); }
    else {
        char vb[2]={opt,0}; var_set(sh,varname,vb,-1);
        if (op[1]==':') {
            if (arg[oc+1]) {
                var_set(sh,"OPTARG",arg+oc+1,-1); optind++;
                var_set(sh,"OPTIND",sh_asprintf("%d",optind),-1);
                var_set(sh,"OPTIND_C","1",-1);
                if (free_args) free(args);
                return 0;
            } else if (optind<nargs) {
                optind++; var_set(sh,"OPTARG",args[optind-1],-1); optind++;
                var_set(sh,"OPTIND",sh_asprintf("%d",optind),-1);
                var_set(sh,"OPTIND_C","1",-1);
                if (free_args) free(args);
                return 0;
            } else { var_set(sh,"OPTARG","",-1); var_set(sh,varname,"?",-1); }
        }
    }
    oc++;
    if (!arg[oc]) {
        optind++; var_set(sh,"OPTIND",sh_asprintf("%d",optind),-1); var_set(sh,"OPTIND_C","1",-1);
    } else var_set(sh,"OPTIND_C",sh_asprintf("%d",oc),-1);
    if (free_args) free(args);
    return 0;
}

static int bi_umask(shell_t *sh, char **argv, int argc) {
    (void)sh;
    if (argc==1) { mode_t m=umask(0); umask(m); printf("%04o\n",(unsigned)m); return 0; }
    umask((mode_t)strtol(argv[1],NULL,8));
    return 0;
}

static int bi_times(shell_t *sh, char **a, int n) {
    (void)sh;(void)a;(void)n;
    printf("0m0.000s 0m0.000s\n0m0.000s 0m0.000s\n"); return 0;
}
static int bi_ulimit(shell_t *sh, char **a, int n) { (void)sh;(void)a;(void)n; return 0; }
static int bi_hash  (shell_t *sh, char **a, int n) { (void)sh;(void)a;(void)n; return 0; }

static int bi_dot(shell_t *sh, char **argv, int argc) {
    if (argc < 2) { sh_error(sh,".: filename argument required"); return 1; }
    FILE *f = fopen(argv[1],"r");
    if (!f) {
        char *path=var_get(sh,"PATH"); char *pc=path?sh_strdup(path):sh_strdup("/bin:/usr/bin");
        char *dir=pc;
        while (dir&&*dir) {
            char *col=strchr(dir,':'); if(col) *col='\0';
            char full[PATH_MAX]; snprintf(full,sizeof full,"%s/%s",*dir?dir:".",argv[1]);
            f=fopen(full,"r"); if(f){dir=NULL;break;}
            dir=col?col+1:NULL;
        }
        free(pc);
    }
    if (!f) { sh_error(sh,"%s: not found",argv[1]); return 1; }
    fseek(f,0,SEEK_END); long fsz=ftell(f); rewind(f);
    char *src=sh_malloc(fsz+1);
    long rd=(long)fread(src,1,fsz,f); src[rd]='\0'; fclose(f);
    char *old=sh->script_name; sh->script_name=sh_strdup(argv[1]);
    node_t *n=parse_string(sh,src); free(src);
    int ret=n?exec_node(sh,n):0;
    node_free(n); free(sh->script_name); sh->script_name=old;
    if (sh->do_return) { ret=sh->returning; sh->do_return=0; sh->returning=0; }
    return ret;
}

static int bi_test(shell_t *sh, char **argv, int argc) {
    int n = argc;
    if (n>0 && !strcmp(argv[0],"[")) {
        if (n<2||strcmp(argv[n-1],"]")) { sh_error(sh,"[: missing ]"); return 2; }
        n--;
    }
    char **a = argv+1; int na = n-1;
    if (na==0) return 1;
    if (na==1) return strlen(a[0])==0?1:0;
    if (na==2) {
        struct stat st;
        if (!strcmp(a[0],"-n")) return strlen(a[1])>0?0:1;
        if (!strcmp(a[0],"-z")) return strlen(a[1])==0?0:1;
        if (!strcmp(a[0],"-e")) return stat(a[1],&st)==0?0:1;
        if (!strcmp(a[0],"-f")) return (stat(a[1],&st)==0&&S_ISREG(st.st_mode))?0:1;
        if (!strcmp(a[0],"-d")) return (stat(a[1],&st)==0&&S_ISDIR(st.st_mode))?0:1;
        if (!strcmp(a[0],"-r")) return access(a[1],R_OK)==0?0:1;
        if (!strcmp(a[0],"-w")) return access(a[1],W_OK)==0?0:1;
        if (!strcmp(a[0],"-x")) return access(a[1],X_OK)==0?0:1;
        if (!strcmp(a[0],"-s")) return (stat(a[1],&st)==0&&st.st_size>0)?0:1;
        if (!strcmp(a[0],"-L")||!strcmp(a[0],"-h")) {
            struct stat lst; return (lstat(a[1],&lst)==0&&S_ISLNK(lst.st_mode))?0:1;
        }
        if (!strcmp(a[0],"-p")) return (stat(a[1],&st)==0&&S_ISFIFO(st.st_mode))?0:1;
        if (!strcmp(a[0],"-c")) return (stat(a[1],&st)==0&&S_ISCHR(st.st_mode))?0:1;
        if (!strcmp(a[0],"-b")) return (stat(a[1],&st)==0&&S_ISBLK(st.st_mode))?0:1;
        if (!strcmp(a[0],"-t")) return isatty(atoi(a[1]))?0:1;
        if (!strcmp(a[0],"!"))  { char *fa[2]={"test",a[1]}; return !bi_test(sh,fa,2); }
        return 1;
    }
    if (na==3) {
        if (!strcmp(a[1],"=")||!strcmp(a[1],"==")) return strcmp(a[0],a[2])==0?0:1;
        if (!strcmp(a[1],"!="))  return strcmp(a[0],a[2])!=0?0:1;
        if (!strcmp(a[1],"-eq")) return atol(a[0])==atol(a[2])?0:1;
        if (!strcmp(a[1],"-ne")) return atol(a[0])!=atol(a[2])?0:1;
        if (!strcmp(a[1],"-lt")) return atol(a[0])< atol(a[2])?0:1;
        if (!strcmp(a[1],"-le")) return atol(a[0])<=atol(a[2])?0:1;
        if (!strcmp(a[1],"-gt")) return atol(a[0])> atol(a[2])?0:1;
        if (!strcmp(a[1],"-ge")) return atol(a[0])>=atol(a[2])?0:1;
        if (!strcmp(a[1],"-nt")) {
            struct stat s1,s2; stat(a[0],&s1); stat(a[2],&s2);
            return s1.st_mtime>s2.st_mtime?0:1;
        }
        if (!strcmp(a[1],"-ot")) {
            struct stat s1,s2; stat(a[0],&s1); stat(a[2],&s2);
            return s1.st_mtime<s2.st_mtime?0:1;
        }
        if (!strcmp(a[1],"-ef")) {
            struct stat s1,s2; stat(a[0],&s1); stat(a[2],&s2);
            return (s1.st_dev==s2.st_dev&&s1.st_ino==s2.st_ino)?0:1;
        }
        if (!strcmp(a[0],"!")) {
            char *fa[3]={"test",a[1],a[2]}; return !bi_test(sh,fa,3);
        }
        if (!strcmp(a[1],"-a")) {
            char *fl[2]={"test",a[0]},*fr[2]={"test",a[2]};
            return (bi_test(sh,fl,2)==0&&bi_test(sh,fr,2)==0)?0:1;
        }
        if (!strcmp(a[1],"-o")) {
            char *fl[2]={"test",a[0]},*fr[2]={"test",a[2]};
            return (bi_test(sh,fl,2)==0||bi_test(sh,fr,2)==0)?0:1;
        }
    }
    if (na==4&&!strcmp(a[0],"!")) {
        char *fa[4]={"test",a[1],a[2],a[3]}; return !bi_test(sh,fa,4);
    }
    return 1;
}

#ifdef USE_JOBCONTROL

static int bi_trap(shell_t *sh, char **argv, int argc) {
    if (argc==1) {
        for (int i=0;i<32;i++)
            if (trap_actions[i]) printf("trap -- '%s' %d\n",trap_actions[i],i);
        return 0;
    }
    char *action=argv[1];
    for (int i=2; i<argc; i++) {
        int sig=-1;
        if (isdigit((unsigned char)argv[i][0])) sig=atoi(argv[i]);
        else if (!strcmp(argv[i],"EXIT")||!strcmp(argv[i],"0")) sig=0;
        else if (!strcmp(argv[i],"INT"))  sig=SIGINT;
        else if (!strcmp(argv[i],"TERM")) sig=SIGTERM;
        else if (!strcmp(argv[i],"HUP"))  sig=SIGHUP;
        else if (!strcmp(argv[i],"QUIT")) sig=SIGQUIT;
        else if (!strcmp(argv[i],"ALRM")) sig=SIGALRM;
        else if (!strcmp(argv[i],"USR1")) sig=SIGUSR1;
        else if (!strcmp(argv[i],"USR2")) sig=SIGUSR2;
        else if (!strcmp(argv[i],"PIPE")) sig=SIGPIPE;
        else if (!strcmp(argv[i],"CHLD")) sig=SIGCHLD;
        else { sh_error(sh,"trap: unknown signal: %s",argv[i]); continue; }
        if (sig<0||sig>=32) continue;
        free(trap_actions[sig]);
        if (!strcmp(action,"-")) {
            trap_actions[sig]=NULL; signal(sig,SIG_DFL);
        } else if (!*action) {
            trap_actions[sig]=sh_strdup(""); signal(sig,SIG_IGN);
        } else {
            trap_actions[sig]=sh_strdup(action);
        }
    }
    return 0;
}

static int bi_kill(shell_t *sh, char **argv, int argc) {
    int sig=SIGTERM, i=1;
    if (i<argc && argv[i][0]=='-') {
        char *sn=argv[i]+1;
        if (!strcmp(sn,"l")) {
            puts("HUP INT QUIT ILL TRAP ABRT BUS FPE KILL USR1 SEGV USR2 PIPE ALRM TERM");
            return 0;
        }
        if (isdigit((unsigned char)*sn)) sig=atoi(sn);
        else if (!strcmp(sn,"TERM")) sig=SIGTERM; else if (!strcmp(sn,"INT"))  sig=SIGINT;
        else if (!strcmp(sn,"KILL")) sig=SIGKILL; else if (!strcmp(sn,"HUP"))  sig=SIGHUP;
        else if (!strcmp(sn,"QUIT")) sig=SIGQUIT; else if (!strcmp(sn,"PIPE")) sig=SIGPIPE;
        else if (!strcmp(sn,"ALRM")) sig=SIGALRM;
        else { sh_error(sh,"kill: unknown signal %s",sn); return 1; }
        i++;
    }
    int ret=0;
    for (; i<argc; i++) {
        if (kill((pid_t)atoi(argv[i]),sig)<0)
            { sh_error(sh,"kill: %s: %s",argv[i],strerror(errno)); ret=1; }
    }
    return ret;
}

static int bi_fg(shell_t *sh, char **argv, int argc) {
    job_update(sh);
    job_t *j=NULL;
    if (argc>1) {
        int jn=atoi(argv[1][0]=='%'?argv[1]+1:argv[1]);
        for (int i=0;i<MAXJOBS;i++) if(sh->jobs[i].active&&sh->jobs[i].jobno==jn){j=&sh->jobs[i];break;}
    } else {
        for (int i=sh->njobs-1;i>=0;i--) if(sh->jobs[i].active){j=&sh->jobs[i];break;}
    }
    if (!j) { sh_error(sh,"fg: no current job"); return 1; }
    fprintf(stderr,"%s\n",j->cmd);
    if (j->state==J_STOP) kill(-j->pgrp,SIGCONT);
    tcsetpgrp(0,j->pgrp);
    int st=job_wait(sh,j->pgrp);
    tcsetpgrp(0,sh->shell_pgid);
    return st;
}

static int bi_bg(shell_t *sh, char **argv, int argc) {
    job_update(sh);
    job_t *j=NULL;
    if (argc>1) {
        int jn=atoi(argv[1][0]=='%'?argv[1]+1:argv[1]);
        for (int i=0;i<MAXJOBS;i++) if(sh->jobs[i].active&&sh->jobs[i].jobno==jn){j=&sh->jobs[i];break;}
    } else {
        for (int i=sh->njobs-1;i>=0;i--) if(sh->jobs[i].active&&sh->jobs[i].state==J_STOP){j=&sh->jobs[i];break;}
    }
    if (!j) { sh_error(sh,"bg: no current job"); return 1; }
    j->state=J_RUN; kill(-j->pgrp,SIGCONT);
    fprintf(stderr,"[%d] %s\n",j->jobno,j->cmd);
    return 0;
}

#endif /* USE_JOBCONTROL */

int exec_builtin(shell_t *sh, char **argv, int argc) {
    if (!argv || !argv[0]) return 1;
    const char *n = argv[0];
    if (!strcmp(n,":"))                 return bi_colon(sh,argv,argc);
    if (!strcmp(n,".")||!strcmp(n,"source")) return bi_dot(sh,argv,argc);
    if (!strcmp(n,"true"))              return bi_true(sh,argv,argc);
    if (!strcmp(n,"false"))             return bi_false(sh,argv,argc);
    if (!strcmp(n,"echo"))              return bi_echo(sh,argv,argc);
    if (!strcmp(n,"printf"))            return bi_printf(sh,argv,argc);
    if (!strcmp(n,"read"))              return bi_read(sh,argv,argc);
    if (!strcmp(n,"cd"))                return bi_cd(sh,argv,argc);
    if (!strcmp(n,"pwd"))               return bi_pwd(sh,argv,argc);
    if (!strcmp(n,"exit"))              return bi_exit(sh,argv,argc);
    if (!strcmp(n,"return"))            return bi_return(sh,argv,argc);
    if (!strcmp(n,"break"))             return bi_break(sh,argv,argc);
    if (!strcmp(n,"continue"))          return bi_continue(sh,argv,argc);
    if (!strcmp(n,"shift"))             return bi_shift(sh,argv,argc);
    if (!strcmp(n,"export"))            return bi_export(sh,argv,argc);
    if (!strcmp(n,"readonly"))          return bi_readonly(sh,argv,argc);
    if (!strcmp(n,"unset"))             return bi_unset(sh,argv,argc);
    if (!strcmp(n,"set"))               return bi_set(sh,argv,argc);
    if (!strcmp(n,"local"))             return bi_local(sh,argv,argc);
    if (!strcmp(n,"eval"))              return bi_eval(sh,argv,argc);
    if (!strcmp(n,"exec"))              return bi_exec(sh,argv,argc);
    if (!strcmp(n,"wait"))              return bi_wait(sh,argv,argc);
    if (!strcmp(n,"jobs"))              return bi_jobs(sh,argv,argc);
    if (!strcmp(n,"type"))              return bi_type(sh,argv,argc);
    if (!strcmp(n,"command"))           return bi_command(sh,argv,argc);
    if (!strcmp(n,"getopts"))           return bi_getopts(sh,argv,argc);
    if (!strcmp(n,"umask"))             return bi_umask(sh,argv,argc);
    if (!strcmp(n,"ulimit"))            return bi_ulimit(sh,argv,argc);
    if (!strcmp(n,"times"))             return bi_times(sh,argv,argc);
    if (!strcmp(n,"hash"))              return bi_hash(sh,argv,argc);
    if (!strcmp(n,"test")||!strcmp(n,"[")) return bi_test(sh,argv,argc);
#ifdef USE_JOBCONTROL
    if (!strcmp(n,"trap"))              return bi_trap(sh,argv,argc);
    if (!strcmp(n,"kill"))              return bi_kill(sh,argv,argc);
    if (!strcmp(n,"fg"))                return bi_fg(sh,argv,argc);
    if (!strcmp(n,"bg"))                return bi_bg(sh,argv,argc);
#endif
    return 127;
}