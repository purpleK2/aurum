#include "aurum.h"

#include "stdio.h"
#include "stdlib.h"
#include "setjmp.h"
#include "stdarg.h"
#include "string.h"
#include "ctype.h"

shell_t *gsh = NULL;
jmp_buf err_jmp;

void sh_error(shell_t *sh, const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "%s: ", sh && sh->script_name ? sh->script_name : "aurum");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
}

void sh_warn(shell_t *sh, const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "%s: ", sh && sh->script_name ? sh->script_name : "aurum");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
}

void *sh_malloc(size_t n) {
    void *p = malloc(n);
    if (!p) { fprintf(stderr, "aurum: out of memory\n"); exit(1); }
    return p;
}

void *sh_realloc(void *p, size_t n) {
    p = realloc(p, n);
    if (!p) { fprintf(stderr, "aurum: out of memory\n"); exit(1); }
    return p;
}

char *sh_strdup(const char *s) {
    if (!s) return NULL;
    size_t l = strlen(s)+1;
    char *d = sh_malloc(l);
    memcpy(d, s, l);
    return d;
}

char *sh_strndup(const char *s, size_t n) {
    char *d = sh_malloc(n+1);
    memcpy(d, s, n);
    d[n] = '\0';
    return d;
}

char *sh_asprintf(const char *fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    char *buf = sh_malloc(n+1);
    vsnprintf(buf, n+1, fmt, ap2);
    va_end(ap2);
    return buf;
}

int is_name(const char *s) {
    if (!s || !*s) return 0;
    if (!isalpha((unsigned char)*s) && *s != '_') return 0;
    for (s++; *s; s++)
        if (!isalnum((unsigned char)*s) && *s != '_') return 0;
    return 1;
}

int is_special_param(char c) {
    return c == '@' || c == '*' || c == '#' || c == '?' ||
           c == '-' || c == '$' || c == '!' || c == '0';
}

char *join_words(char **words, int n, const char *sep) {
    if (!n) return sh_strdup("");
    size_t total = 0;
    int sl = strlen(sep);
    for (int i = 0; i < n; i++) total += strlen(words[i]);
    total += sl * (n > 1 ? n-1 : 0) + 1;
    char *buf = sh_malloc(total);
    buf[0] = '\0';
    for (int i = 0; i < n; i++) {
        if (i) strcat(buf, sep);
        strcat(buf, words[i]);
    }
    return buf;
}

char *token_type_str(token_type t) {
    switch(t) {
    case TK_WORD:         return "WORD";
    case TK_ASSIGN:       return "ASSIGN";
    case TK_PIPE:         return "|";
    case TK_AND:          return "&&";
    case TK_OR:           return "||";
    case TK_SEMI:         return ";";
    case TK_AMP:          return "&";
    case TK_NEWLINE:      return "NEWLINE";
    case TK_redir_IN:     return "<";
    case TK_redir_OUT:    return ">";
    case TK_redir_APP:    return ">>";
    case TK_redir_HER:    return "<<";
    case TK_redir_HED:    return "<<-";
    case TK_redir_DUP_IN: return "<&";
    case TK_redir_DUP_OUT:return ">&";
    case TK_redir_CLOBBER:return ">|";
    case TK_LPAREN:       return "(";
    case TK_RPAREN:       return ")";
    case TK_LBRACE:       return "{";
    case TK_RBRACE:       return "}";
    case TK_BANG:         return "!";
    case TK_IF:           return "if";
    case TK_THEN:         return "then";
    case TK_ELSE:         return "else";
    case TK_ELIF:         return "elif";
    case TK_FI:           return "fi";
    case TK_WHILE:        return "while";
    case TK_UNTIL:        return "until";
    case TK_DO:           return "do";
    case TK_DONE:         return "done";
    case TK_FOR:          return "for";
    case TK_IN:           return "in";
    case TK_CASE:         return "case";
    case TK_ESAC:         return "esac";
    case TK_FUNCTION:     return "function";
    case TK_EOF:          return "EOF";
    default:              return "?";
    }
}