#include "aurum.h"

#include "lexer.h"
#include "token.h"
#include "util.h"
#include "string.h"
#include "stdlib.h"
#include "input.h"
#include "ctype.h"

static int lexer_getc(lexer_t *l) {
    if (l->pos >= l->len) return EOF;
    char c = l->buf[l->pos++];
    if (c == '\r') {
        if (l->pos < l->len && l->buf[l->pos] == '\n') {
            c = l->buf[l->pos++];
        } else {
            c = '\n';
        }
    }
    if (c == '\n') l->lineno++;
    return (unsigned char)c;
}
static void lexer_ungetc(lexer_t *l, int c) {
    if (l->pos > 0) {
        l->pos--;
        if (c == '\n') l->lineno--;
    }
}
static int lexer_peekc(lexer_t *l) {
    if (l->pos >= l->len) return EOF;
    return (unsigned char)l->buf[l->pos];
}

typedef struct { char *s; size_t n; size_t cap; } Buf;
static void buf_init(Buf *b) { b->s=sh_malloc(64); b->n=0; b->cap=64; b->s[0]='\0'; }
static void buf_push(Buf *b, char c) {
    if (b->n+2 > b->cap) { b->cap*=2; b->s=sh_realloc(b->s, b->cap); }
    b->s[b->n++] = c;
    b->s[b->n] = '\0';
}
static void buf_pushstr(Buf *b, const char *s) { while(*s) buf_push(b, *s++); }
static char *buf_done(Buf *b) { return b->s; }

lexer_t *lexer_new(shell_t *sh, const char *src, int interactive) {
    lexer_t *l = sh_malloc(sizeof *l);
    memset(l, 0, sizeof *l);
    l->sh = sh;
    l->interactive = interactive;
    l->lineno = 1;
    l->start_of_cmd = 1;
    if (src) {
        l->len = strlen(src);
        l->buf = sh_malloc(l->len + 2);
        memcpy(l->buf, src, l->len);
        l->buf[l->len] = '\0';
    }
    return l;
}

void lexer_free(lexer_t *l) {
    if (!l) return;
    free(l->buf);
    if (l->peeked) token_free(l->peeked);
    for (int i = 0; i < l->nhere; i++) {
        free(l->here[i].delim);
        free(l->here[i].body);
    }
    free(l);
}

void token_free(token_t *t) {
    if (!t) return;
    free(t->val);
    free(t);
}

static token_t *mktok(token_type type, const char *val, int lineno) {
    token_t *t = sh_malloc(sizeof *t);
    t->type = type;
    t->val = val ? sh_strdup(val) : NULL;
    t->lineno = lineno;
    return t;
}

static void lex_dollar_paren(lexer_t *l, Buf *b);
static void lex_dollar_brace(lexer_t *l, Buf *b);

static void lex_single_quote(lexer_t *l, Buf *b) {
    int c;
    while ((c = lexer_getc(l)) != EOF) {
        if (c == '\'') return;
        buf_push(b, c);
    }
    sh_error(l->sh, "line %d: unterminated single quote", l->lineno);
}

static void lex_double_quote(lexer_t *l, Buf *b) {
    buf_push(b, '"');
    int c;
    while ((c = lexer_getc(l)) != EOF) {
        if (c == '"') { buf_push(b, '"'); return; }
        if (c == '\\') {
            int c2 = lexer_getc(l);
            if (c2 == '$' || c2 == '`' || c2 == '"' || c2 == '\\' || c2 == '\n') {
                if (c2 == '\n') { continue; }
                buf_push(b, '\\'); buf_push(b, c2);
            } else {
                buf_push(b, '\\'); buf_push(b, c2);
            }
        } else if (c == '$') {
            buf_push(b, '$');
            int nc = lexer_peekc(l);
            if (nc == '(') {
                lexer_getc(l);
                lex_dollar_paren(l, b);
            } else if (nc == '{') {
                lexer_getc(l);
                lex_dollar_brace(l, b);
            }
        } else if (c == '`') {
            buf_push(b, '`');
            int depth = 1;
            while ((c = lexer_getc(l)) != EOF) {
                if (c == '`') { buf_push(b, '`'); break; }
                if (c == '\\') {
                    buf_push(b, c);
                    c = lexer_getc(l);
                    if (c != EOF) buf_push(b, c);
                } else buf_push(b, c);
            }
        } else {
            buf_push(b, c);
        }
    }
    sh_error(l->sh, "line %d: unterminated double quote", l->lineno);
}

static void lex_dollar_paren(lexer_t *l, Buf *b) {
    buf_push(b, '(');
    int depth = 1, c;
    while ((c = lexer_getc(l)) != EOF) {
        if (c == '(') { buf_push(b, c); depth++; }
        else if (c == ')') {
            if (--depth == 0) { buf_push(b, c); return; }
            buf_push(b, c);
        } else if (c == '\'') {
            buf_push(b, c);
            while ((c = lexer_getc(l)) != EOF) {
                buf_push(b, c);
                if (c == '\'') break;
            }
        } else if (c == '"') {
            lex_double_quote(l, b);
        } else if (c == '\\') {
            buf_push(b, c);
            c = lexer_getc(l);
            if (c != EOF) buf_push(b, c);
        } else {
            buf_push(b, c);
        }
    }
}

static void lex_dollar_brace(lexer_t *l, Buf *b) {
    buf_push(b, '{');
    int depth = 1, c;
    while ((c = lexer_getc(l)) != EOF) {
        buf_push(b, c);
        if (c == '{') depth++;
        else if (c == '}') { if (--depth == 0) return; }
        else if (c == '\\') { c = lexer_getc(l); if (c!=EOF) buf_push(b,c); }
        else if (c == '\'') {
            while((c=lexer_getc(l))!=EOF){ buf_push(b,c); if(c=='\'') break; }
        } else if (c == '"') {
            Buf inner; buf_init(&inner);
            lex_double_quote(l, &inner);
            buf_pushstr(b, inner.s); free(inner.s);
        }
    }
}

static token_type keyword_always(const char *s) {
    if (!strcmp(s,"if"))       return TK_IF;
    if (!strcmp(s,"then"))     return TK_THEN;
    if (!strcmp(s,"else"))     return TK_ELSE;
    if (!strcmp(s,"elif"))     return TK_ELIF;
    if (!strcmp(s,"fi"))       return TK_FI;
    if (!strcmp(s,"while"))    return TK_WHILE;
    if (!strcmp(s,"until"))    return TK_UNTIL;
    if (!strcmp(s,"do"))       return TK_DO;
    if (!strcmp(s,"done"))     return TK_DONE;
    if (!strcmp(s,"for"))      return TK_FOR;
    if (!strcmp(s,"in"))       return TK_IN;
    if (!strcmp(s,"case"))     return TK_CASE;
    if (!strcmp(s,"esac"))     return TK_ESAC;
    if (!strcmp(s,"function")) return TK_FUNCTION;
    return TK_WORD;
}

static token_type keyword_at_cmd(const char *s) {
    token_type t = keyword_always(s);
    if (t != TK_WORD) return t;
    if (!strcmp(s,"{"))  return TK_LBRACE;
    if (!strcmp(s,"}"))  return TK_RBRACE;
    if (!strcmp(s,"!"))  return TK_BANG;
    return TK_WORD;
}

static void collect_heredocs(lexer_t *l) {
    for (int i = 0; i < l->nhere; i++) {
        Buf b; buf_init(&b);
        char *delim = l->here[i].delim;
        int strip = l->here[i].strip_tabs;
        for (;;) {
            if (l->interactive) {
                char *line = read_line(l->sh, "> ");
                if (!line) { break; }
                char *check = line;
                if (strip) while (*check == '\t') check++;
                if (strcmp(check, delim) == 0) { free(line); break; }
                if (strip) {
                    char *p = line;
                    while (*p == '\t') p++;
                    buf_pushstr(&b, p);
                } else {
                    buf_pushstr(&b, line);
                }
                buf_push(&b, '\n');
                free(line);
            } else {
                Buf line; buf_init(&line);
                int c;
                while ((c = lexer_getc(l)) != EOF && c != '\n') buf_push(&line, c);
                if (c == '\n') l->lineno++;
                char *ln = line.s;
                if (strip) while (*ln == '\t') ln++;
                if (strcmp(ln, delim) == 0) { free(line.s); break; }
                if (strip) {
                    char *p = line.s;
                    while (*p == '\t') p++;
                    buf_pushstr(&b, p);
                } else {
                    buf_pushstr(&b, line.s);
                }
                buf_push(&b, '\n');
                free(line.s);
                if (c == EOF) break;
            }
        }
        l->here[i].body = buf_done(&b);
    }
}

token_t *lexer_next(lexer_t *l) {
    if (l->peeked) {
        token_t *t = l->peeked;
        l->peeked = NULL;
        return t;
    }

    int c;
retry:
    while ((c = lexer_getc(l)) != EOF && (c == ' ' || c == '\t')) {}

    if (c == EOF) return mktok(TK_EOF, NULL, l->lineno);

    int ln = l->lineno;

    if (c == '#') {
        while ((c = lexer_getc(l)) != EOF && c != '\n') {}
        if (c == '\n') {
        } else {
            return mktok(TK_EOF, NULL, ln);
        }
    }

    if (c == '\n') {
        if (l->nhere > 0) {
            collect_heredocs(l);
        }
        l->start_of_cmd = 1;
        return mktok(TK_NEWLINE, "\n", ln);
    }

    if (c == '|') {
        int c2 = lexer_peekc(l);
        if (c2 == '|') { lexer_getc(l); l->start_of_cmd=1; return mktok(TK_OR,"||",ln); }
        l->start_of_cmd = 1;
        return mktok(TK_PIPE, "|", ln);
    }
    if (c == '&') {
        int c2 = lexer_peekc(l);
        if (c2 == '&') { lexer_getc(l); l->start_of_cmd=1; return mktok(TK_AND,"&&",ln); }
        l->start_of_cmd = 1;
        return mktok(TK_AMP, "&", ln);
    }
    if (c == ';') {
        int c2 = lexer_peekc(l);
        if (c2 == ';') { lexer_getc(l); l->start_of_cmd=1; return mktok(TK_SEMI,";;",ln); }
        l->start_of_cmd = 1;
        return mktok(TK_SEMI, ";", ln);
    }
    if (c == '(') { l->start_of_cmd=1; return mktok(TK_LPAREN,"(",ln); }
    if (c == ')') { l->start_of_cmd=0; return mktok(TK_RPAREN,")",ln); }
    if (c == '{') { l->start_of_cmd=1; return mktok(TK_LBRACE,"{",ln); }
    if (c == '}') { l->start_of_cmd=1; return mktok(TK_RBRACE,"}",ln); }

    if (c == '<') {
        int c2 = lexer_peekc(l);
        if (c2 == '<') {
            lexer_getc(l);
            int c3 = lexer_peekc(l);
            if (c3 == '-') { lexer_getc(l); return mktok(TK_redir_HED,"<<-",ln); }
            return mktok(TK_redir_HER,"<<",ln);
        }
        if (c2 == '&') { lexer_getc(l); return mktok(TK_redir_DUP_IN,"<&",ln); }
        return mktok(TK_redir_IN,"<",ln);
    }
    if (c == '>') {
        int c2 = lexer_peekc(l);
        if (c2 == '>') { lexer_getc(l); return mktok(TK_redir_APP,">>",ln); }
        if (c2 == '&') { lexer_getc(l); return mktok(TK_redir_DUP_OUT,">&",ln); }
        if (c2 == '|') { lexer_getc(l); return mktok(TK_redir_CLOBBER,">|",ln); }
        return mktok(TK_redir_OUT,">",ln);
    }

    if (c == '\\') {
        int c2 = lexer_peekc(l);
        if (c2 == '\n') { lexer_getc(l); goto retry; }
        lexer_ungetc(l, c);
    } else {
        lexer_ungetc(l, c);
    }

    {
        Buf b; buf_init(&b);
        int quoted = 0;
        int first = 1;

        size_t save_pos = l->pos;
        int save_ln = l->lineno;
        c = lexer_getc(l);
        if (isdigit(c)) {
            Buf digits; buf_init(&digits);
            buf_push(&digits, c);
            while (isdigit(lexer_peekc(l))) buf_push(&digits, lexer_getc(l));
            int nc = lexer_peekc(l);
            if (nc == '>' || nc == '<') {
                char *ds = buf_done(&digits);
                token_t *t = mktok(TK_WORD, ds, ln);
                free(ds);
                return t;
            }
            buf_pushstr(&b, digits.s); free(digits.s);
        } else {
            lexer_ungetc(l, c);
        }

        int in_dquote = 0;
        for (;;) {
            c = lexer_getc(l);
            if (c == EOF) break;
            if (!in_dquote && (c == ' ' || c == '\t' || c == '\n' ||
                c == ';' || c == '&' || c == '|' ||
                c == '(' || c == ')' ||
                c == '<' || c == '>' ||
                (c == '#' && !(b.n > 0 && b.s[b.n-1] == '$')))) {
                lexer_ungetc(l, c);
                break;
            }
            if (c == '\\') {
                int c2 = lexer_getc(l);
                if (c2 == '\n') continue;
                buf_push(&b, '\\');
                if (c2 != EOF) buf_push(&b, c2);
                quoted = 1;
            } else if (c == '\'') {
                Buf sq; buf_init(&sq);
                lex_single_quote(l, &sq);
                buf_push(&b, '\x01');
                buf_pushstr(&b, sq.s);
                buf_push(&b, '\x01');
                free(sq.s);
                quoted = 1;
            } else if (c == '"') {
                in_dquote = 1;
                lex_double_quote(l, &b);
                in_dquote = 0;
                quoted = 1;
            } else if (c == '$') {
                buf_push(&b, '$');
                int nc = lexer_peekc(l);
                if (nc == '(') {
                    lexer_getc(l);
                    lex_dollar_paren(l, &b);
                } else if (nc == '{') {
                    lexer_getc(l);
                    lex_dollar_brace(l, &b);
                } else if (nc == '$' || nc == '?' || nc == '!' ||
                           nc == '@' || nc == '*' || nc == '#' ||
                           nc == '-' || nc == '_' || isalnum(nc)) {
                }
            } else if (c == '`') {
                buf_push(&b, '`');
                while ((c = lexer_getc(l)) != EOF) {
                    buf_push(&b, c);
                    if (c == '`') break;
                    if (c == '\\') {
                        c = lexer_getc(l);
                        if (c != EOF) buf_push(&b, c);
                    }
                }
                quoted = 1;
            } else {
                buf_push(&b, c);
            }
            first = 0;
        }

        if (b.n == 0) { free(b.s); goto retry; }

        char *word = buf_done(&b);

        if (!quoted) {
            token_type kw = l->start_of_cmd ? keyword_at_cmd(word) : keyword_always(word);
            if (kw != TK_WORD) {
                token_t *t = mktok(kw, word, ln);
                free(word);
                return t;
            }
        }

        {
            char *eq = strchr(word, '=');
            if (eq && eq != word) {
                int ok = 1;
                for (char *p = word; p < eq; p++) {
                    if (!isalnum((unsigned char)*p) && *p != '_') { ok=0; break; }
                    if ((unsigned char)*p == 0x01 || (unsigned char)*p == 0x03) { ok=0; break; }
                }
                if (ok && (word[0] == '_' || isalpha((unsigned char)word[0]))) {
                    token_t *t = mktok(TK_ASSIGN, word, ln);
                    free(word);
                    return t;
                }
            }
        }

        l->start_of_cmd = 0;
        token_t *t = mktok(TK_WORD, word, ln);
        free(word);
        return t;
    }
}

token_t *lexer_peek(lexer_t *l) {
    if (!l->peeked) l->peeked = lexer_next(l);
    return l->peeked;
}

void lexer_push_heredoc(lexer_t *l, const char *delim, int strip_tabs) {
    if (l->nhere >= 16) { sh_error(l->sh, "too many heredocs"); return; }
    Buf b; buf_init(&b);
    for (const char *p = delim; *p; p++) {
        if (*p == '\'' || *p == '"' || *p == '\\') continue;
        buf_push(&b, *p);
    }
    l->here[l->nhere].delim = buf_done(&b);
    l->here[l->nhere].strip_tabs = strip_tabs;
    l->here[l->nhere].body = NULL;
    l->nhere++;
}

char *lexer_get_heredoc(lexer_t *l, int i) {
    if (i < 0 || i >= l->nhere) return sh_strdup("");
    return sh_strdup(l->here[i].body ? l->here[i].body : "");
}

int lexer_heredoc_count(lexer_t *l) { return l->nhere; }