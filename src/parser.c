#include "aurum.h"

#include "parser.h"
#include "lexer.h"
#include "token.h"
#include "util.h"
#include "string.h"
#include "ctype.h"
#include "stdlib.h"


void    lexer_push_heredoc(lexer_t *l, const char *delim, int strip_tabs);
char   *lexer_get_heredoc(lexer_t *l, int i);
int     lexer_heredoc_count(lexer_t *l);
token_t  *lexer_peek(lexer_t *l);

static node_t *parse_list(parser_t *p);
static node_t *parse_pipeline(parser_t *p);
static node_t *parse_command(parser_t *p);
static node_t *parse_compound(parser_t *p);
static node_t *parse_simple(parser_t *p);
static redir_t *parse_redirs(parser_t *p, node_t *n);

static node_t *node_new(node_type t) {
    node_t *n = sh_malloc(sizeof *n);
    memset(n, 0, sizeof *n);
    n->type = t;
    return n;
}

void node_free(node_t *n) {
    if (!n) return;
    switch (n->type) {
    case N_CMD:
        for (int i = 0; i < n->argc; i++) free(n->argv[i]);
        free(n->argv);
        for (int i = 0; i < n->nassigns; i++) free(n->assigns[i]);
        free(n->assigns);
        for (redir_t *r = n->redirs, *nx; r; r = nx) {
            nx = r->next; free(r->target); free(r);
        }
        break;
    case N_PIPE:
        for (int i = 0; i < n->ncmds; i++) node_free(n->cmds[i]);
        free(n->cmds);
        break;
    case N_LIST: case N_AND_OR:
        node_free(n->left); node_free(n->right); break;
    case N_IF:
        node_free(n->cond); node_free(n->body); node_free(n->els); break;
    case N_WHILE: case N_UNTIL:
        node_free(n->cond); node_free(n->body); break;
    case N_FOR:
        free(n->name);
        for (int i = 0; i < n->nwords; i++) free(n->words[i]);
        free(n->words);
        node_free(n->body);
        break;
    case N_CASE:
        free(n->name);
        node_free(n->cases);
        break;
    case N_CASE_ITEM:
        for (int i = 0; i < n->npats; i++) free(n->patterns[i]);
        free(n->patterns);
        node_free(n->action);
        node_free(n->next_case);
        break;
    case N_SUBSHELL: case N_BRACE: case N_BANG:
        node_free(n->inner);
        for (redir_t *r = n->redirs, *nx; r; r = nx) {
            nx = r->next; free(r->target); free(r);
        }
        break;
    case N_FUNC:
        free(n->name); node_free(n->func_body); break;
    case N_redir:
        node_free(n->inner);
        for (redir_t *r = n->redirs, *nx; r; r = nx) {
            nx = r->next; free(r->target); free(r);
        }
        break;
    }
    free(n);
}

static token_t *advance(parser_t *p) {
    token_free(p->cur);
    p->cur = lexer_next(p->l);
    return p->cur;
}

static token_t *peek(parser_t *p) {
    return lexer_peek(p->l);
}

static int check(parser_t *p, token_type t) {
    return p->cur && p->cur->type == t;
}

static int match(parser_t *p, token_type t) {
    if (check(p, t)) { advance(p); return 1; }
    return 0;
}

static void expect(parser_t *p, token_type t) {
    if (!check(p, t)) {
        sh_error(p->sh, "line %d: expected '%s', got '%s' ('%s')",
            p->cur ? p->cur->lineno : 0,
            token_type_str(t),
            p->cur ? token_type_str(p->cur->type) : "EOF",
            p->cur && p->cur->val ? p->cur->val : "");
        longjmp(err_jmp, 1);
    }
    advance(p);
}

static void skip_newlines(parser_t *p) {
    while (check(p, TK_NEWLINE)) advance(p);
}

static int is_redir_tok(token_type t) {
    return t == TK_redir_IN || t == TK_redir_OUT || t == TK_redir_APP ||
           t == TK_redir_HER || t == TK_redir_HED || t == TK_redir_DUP_IN ||
           t == TK_redir_DUP_OUT || t == TK_redir_CLOBBER;
}

static redir_t *parse_one_redir(parser_t *p, int fd_override, int *heredoc_idx) {
    redir_t *r = sh_malloc(sizeof *r);
    memset(r, 0, sizeof *r);
    token_type op = p->cur->type;
    r->op = op;
    advance(p);

    if (fd_override >= 0) r->fd = fd_override;
    else {
        switch (op) {
        case TK_redir_IN: case TK_redir_HER: case TK_redir_HED:
        case TK_redir_DUP_IN: r->fd = 0; break;
        default: r->fd = 1; break;
        }
    }

    if (!check(p, TK_WORD) && !check(p, TK_ASSIGN)) {
        sh_error(p->sh, "line %d: expected redirect target", p->cur ? p->cur->lineno : 0);
        free(r); longjmp(err_jmp, 1);
    }
    r->target = sh_strdup(p->cur->val);
    if (op == TK_redir_HER || op == TK_redir_HED) {
        int strip = (op == TK_redir_HED);
        int idx = lexer_heredoc_count(p->l);
        lexer_push_heredoc(p->l, r->target, strip);
        free(r->target);
        r->target = sh_asprintf("\x02%d\x02", idx);
    }
    advance(p);
    return r;
}

static redir_t *collect_redirs(parser_t *p) {
    redir_t *head = NULL, **tail = &head;
    while (1) {
        int fd_override = -1;
        if (check(p, TK_WORD) && p->cur->val) {
            char *v = p->cur->val;
            int all_dig = 1;
            for (char *c = v; *c; c++) if (!isdigit((unsigned char)*c)) { all_dig=0; break; }
            if (all_dig && *v) {
                token_t *nxt = peek(p);
                if (nxt && is_redir_tok(nxt->type)) {
                    fd_override = atoi(v);
                    advance(p);
                } else break;
            } else break;
        } else if (is_redir_tok(p->cur->type)) {

        } else break;

        if (!is_redir_tok(p->cur->type)) break;
        redir_t *r = parse_one_redir(p, fd_override, NULL);
        *tail = r; tail = &r->next;
    }
    return head;
}

static node_t *parse_simple(parser_t *p) {
    node_t *n = node_new(N_CMD);
    int asize = 8, argc = 0;
    char **argv = sh_malloc(asize * sizeof(char*));
    int nass = 0, asssize = 4;
    char **assigns = sh_malloc(asssize * sizeof(char*));
    redir_t *redirs = NULL, **rtail = &redirs;

    while (1) {
        if (check(p, TK_ASSIGN) && argc == 0) {
            if (nass >= asssize) { asssize*=2; assigns=sh_realloc(assigns, asssize*sizeof(char*)); }
            assigns[nass++] = sh_strdup(p->cur->val);
            advance(p);
            continue;
        }
        if (is_redir_tok(p->cur->type) ||
            (check(p, TK_WORD) && p->cur->val && p->cur->val[0] && (
                (strlen(p->cur->val) <= 4) &&
                ((strspn(p->cur->val,"0123456789")==strlen(p->cur->val)))
            ) && peek(p) && is_redir_tok(peek(p)->type))) {
            redir_t *r = NULL;
            int fd_override = -1;
            if (check(p, TK_WORD)) {
                fd_override = atoi(p->cur->val);
                advance(p);
            }
            r = parse_one_redir(p, fd_override, NULL);
            *rtail = r; rtail = &r->next;
            continue;
        }
        if (check(p, TK_WORD)) {
            if (argc >= asize-1) { asize*=2; argv=sh_realloc(argv, asize*sizeof(char*)); }
            argv[argc++] = sh_strdup(p->cur->val);
            advance(p);
            continue;
        }
        if (check(p, TK_ASSIGN) && argc > 0) {
            if (argc >= asize-1) { asize*=2; argv=sh_realloc(argv, asize*sizeof(char*)); }
            argv[argc++] = sh_strdup(p->cur->val);
            advance(p);
            continue;
        }
        if (argc > 0 && p->cur && (
            p->cur->type == TK_IF    || p->cur->type == TK_THEN ||
            p->cur->type == TK_ELSE  || p->cur->type == TK_ELIF ||
            p->cur->type == TK_FI    || p->cur->type == TK_WHILE ||
            p->cur->type == TK_UNTIL || p->cur->type == TK_FOR  ||
            p->cur->type == TK_DO    || p->cur->type == TK_DONE ||
            p->cur->type == TK_CASE  || p->cur->type == TK_ESAC ||
            p->cur->type == TK_IN    || p->cur->type == TK_FUNCTION ||
            p->cur->type == TK_BANG)) {
            if (argc >= asize-1) { asize*=2; argv=sh_realloc(argv, asize*sizeof(char*)); }
            argv[argc++] = sh_strdup(p->cur->val);
            advance(p);
            continue;
        }
        break;
    }
    argv[argc] = NULL;
    n->argv = argv; n->argc = argc;
    n->assigns = assigns; n->nassigns = nass;
    n->redirs = redirs;

    redir_t *trail = collect_redirs(p);
    if (trail) { *rtail = trail; }

    return n;
}

static node_t *parse_if(parser_t *p) {
    advance(p);
    skip_newlines(p);

    node_t *root = node_new(N_IF);
    root->cond = parse_list(p);
    skip_newlines(p);
    expect(p, TK_THEN);
    skip_newlines(p);
    root->body = parse_list(p);
    skip_newlines(p);

    node_t *tail = root;
    while (check(p, TK_ELIF)) {
        advance(p);
        skip_newlines(p);
        node_t *elif = node_new(N_IF);
        elif->cond = parse_list(p);
        skip_newlines(p);
        expect(p, TK_THEN);
        skip_newlines(p);
        elif->body = parse_list(p);
        skip_newlines(p);
        tail->els = elif;
        tail = elif;
    }

    if (match(p, TK_ELSE)) {
        skip_newlines(p);
        tail->els = parse_list(p);
        skip_newlines(p);
    }

    expect(p, TK_FI);
    return root;
}

static node_t *parse_while_until(parser_t *p) {
    node_type t = check(p, TK_WHILE) ? N_WHILE : N_UNTIL;
    advance(p);
    skip_newlines(p);
    node_t *n = node_new(t);
    n->cond = parse_list(p);
    skip_newlines(p);
    expect(p, TK_DO);
    skip_newlines(p);
    n->body = parse_list(p);
    skip_newlines(p);
    expect(p, TK_DONE);
    return n;
}

static node_t *parse_for(parser_t *p) {
    advance(p);
    skip_newlines(p);
    if (!check(p, TK_WORD)) {
        sh_error(p->sh, "line %d: expected variable name after 'for'", p->cur ? p->cur->lineno : 0);
        longjmp(err_jmp, 1);
    }
    node_t *n = node_new(N_FOR);
    n->name = sh_strdup(p->cur->val);
    advance(p);
    skip_newlines(p);

    int wsize = 8;
    n->words = sh_malloc(wsize * sizeof(char*));
    n->nwords = 0;

    if (match(p, TK_IN)) {
        while (check(p, TK_WORD)) {
            if (n->nwords >= wsize-1) { wsize*=2; n->words=sh_realloc(n->words, wsize*sizeof(char*)); }
            n->words[n->nwords++] = sh_strdup(p->cur->val);
            advance(p);
        }
        if (check(p, TK_SEMI)) advance(p);
        skip_newlines(p);
    } else {
        if (check(p, TK_SEMI)) advance(p);
        skip_newlines(p);
    }
    n->words[n->nwords] = NULL;

    expect(p, TK_DO);
    skip_newlines(p);
    n->body = parse_list(p);
    skip_newlines(p);
    expect(p, TK_DONE);
    return n;
}

static node_t *parse_case(parser_t *p) {
    advance(p);
    skip_newlines(p);
    if (!check(p, TK_WORD)) {
        sh_error(p->sh, "expected word after 'case'");
        longjmp(err_jmp, 1);
    }
    node_t *n = node_new(N_CASE);
    n->name = sh_strdup(p->cur->val);
    advance(p);
    skip_newlines(p);
    expect(p, TK_IN);
    skip_newlines(p);

    node_t *head = NULL, **ctail = &head;
    while (!check(p, TK_ESAC) && !check(p, TK_EOF)) {
        skip_newlines(p);
        if (check(p, TK_ESAC)) break;
        if (check(p, TK_LPAREN)) advance(p);
        node_t *item = node_new(N_CASE_ITEM);
        int psize = 4;
        item->patterns = sh_malloc(psize * sizeof(char*));
        item->npats = 0;
        while (p->cur && p->cur->type != TK_RPAREN &&
               p->cur->type != TK_EOF && p->cur->type != TK_ESAC) {
            if (p->cur->type == TK_NEWLINE) break;
            if (p->cur->type == TK_SEMI) break;
            if (item->npats >= psize-1) { psize*=2; item->patterns=sh_realloc(item->patterns,psize*sizeof(char*)); }
            item->patterns[item->npats++] = sh_strdup(p->cur->val ? p->cur->val : "");
            advance(p);
            if (check(p, TK_PIPE)) advance(p);
            else break;
        }
        item->patterns[item->npats] = NULL;
        expect(p, TK_RPAREN);
        skip_newlines(p);
        if (check(p, TK_SEMI) && p->cur->val && strcmp(p->cur->val,";;") == 0) {
            item->action = NULL;
        } else if (!check(p, TK_ESAC)) {
            item->action = parse_list(p);
        }
        skip_newlines(p);
        if (check(p, TK_SEMI) && p->cur->val && strcmp(p->cur->val,";;") == 0) advance(p);
        skip_newlines(p);
        *ctail = item; ctail = &item->next_case;
    }
    expect(p, TK_ESAC);
    n->cases = head;
    return n;
}

static node_t *parse_brace_group(parser_t *p) {
    advance(p);
    skip_newlines(p);
    node_t *n = node_new(N_BRACE);
    n->inner = parse_list(p);
    skip_newlines(p);
    expect(p, TK_RBRACE);
    n->redirs = collect_redirs(p);
    return n;
}

static node_t *parse_subshell(parser_t *p) {
    advance(p);
    skip_newlines(p);
    node_t *n = node_new(N_SUBSHELL);
    n->inner = parse_list(p);
    skip_newlines(p);
    expect(p, TK_RPAREN);
    n->redirs = collect_redirs(p);
    return n;
}

static int is_func_def(parser_t *p) {
    if (check(p, TK_FUNCTION)) return 1;
    if (!check(p, TK_WORD)) return 0;
    token_t *nxt = peek(p);
    return nxt && nxt->type == TK_LPAREN;
}

static node_t *parse_func(parser_t *p) {
    node_t *n = node_new(N_FUNC);
    if (match(p, TK_FUNCTION)) {
        skip_newlines(p);
        if (!check(p, TK_WORD)) {
            sh_error(p->sh, "expected function name");
            longjmp(err_jmp, 1);
        }
        n->name = sh_strdup(p->cur->val);
        advance(p);
        if (check(p, TK_LPAREN)) { advance(p); expect(p, TK_RPAREN); }
    } else {
        n->name = sh_strdup(p->cur->val);
        advance(p);
        expect(p, TK_LPAREN);
        expect(p, TK_RPAREN);
    }
    skip_newlines(p);
    n->func_body = parse_compound(p);
    if (!n->func_body) {
        sh_error(p->sh, "expected compound command in function body");
        longjmp(err_jmp, 1);
    }
    n->redirs = collect_redirs(p);
    return n;
}

static node_t *parse_compound(parser_t *p) {
    if (check(p, TK_IF))                 return parse_if(p);
    if (check(p, TK_WHILE)||check(p,TK_UNTIL)) return parse_while_until(p);
    if (check(p, TK_FOR))                return parse_for(p);
    if (check(p, TK_CASE))               return parse_case(p);
    if (check(p, TK_LBRACE))             return parse_brace_group(p);
    if (check(p, TK_LPAREN))             return parse_subshell(p);
    return NULL;
}

static node_t *parse_pipeline(parser_t *p) {
    int bang = 0;
    if (check(p, TK_BANG)) { bang = 1; advance(p); }

    node_t **cmds = sh_malloc(8 * sizeof(node_t*));
    int ncmds = 0, cap = 8;

    node_t *cmd = NULL;
    if (is_func_def(p)) cmd = parse_func(p);
    else {
        cmd = parse_compound(p);
        if (!cmd) cmd = parse_simple(p);
    }
    if (!cmd) {
        free(cmds);
        return NULL;
    }
    cmds[ncmds++] = cmd;

    while (check(p, TK_PIPE)) {
        advance(p);
        skip_newlines(p);
        if (ncmds >= cap-1) { cap*=2; cmds=sh_realloc(cmds, cap*sizeof(node_t*)); }
        node_t *next = NULL;
        if (is_func_def(p)) next = parse_func(p);
        else {
            next = parse_compound(p);
            if (!next) next = parse_simple(p);
        }
        if (!next) {
            sh_error(p->sh, "expected command after '|'");
            for (int i=0;i<ncmds;i++) node_free(cmds[i]);
            free(cmds); longjmp(err_jmp, 1);
        }
        cmds[ncmds++] = next;
    }

    if (ncmds == 1 && !bang) {
        node_t *r = cmds[0];
        free(cmds);
        return r;
    }

    node_t *n = node_new(N_PIPE);
    n->cmds = cmds;
    n->ncmds = ncmds;
    n->pipe_bang = bang;
    return n;
}

static node_t *parse_and_or(parser_t *p) {
    node_t *left = parse_pipeline(p);
    if (!left) return NULL;
    while (check(p, TK_AND) || check(p, TK_OR)) {
        token_type op = p->cur->type;
        advance(p);
        skip_newlines(p);
        node_t *right = parse_pipeline(p);
        if (!right) {
            sh_error(p->sh, "expected command after '%s'", op==TK_AND?"&&":"||");
            node_free(left); longjmp(err_jmp, 1);
        }
        node_t *n = node_new(N_AND_OR);
        n->left = left; n->right = right; n->op = op;
        left = n;
    }
    return left;
}

static node_t *parse_list(parser_t *p) {
    node_t *left = parse_and_or(p);
    if (!left) return NULL;

    while (check(p, TK_SEMI) || check(p, TK_AMP) || check(p, TK_NEWLINE)) {
        token_type op = p->cur->type;
        char *val = p->cur->val ? sh_strdup(p->cur->val) : NULL;
        advance(p);
        skip_newlines(p);
        if (val && strcmp(val,";;") == 0) {
            free(val);
            break;
        }
        free(val);

        if (op == TK_SEMI || op == TK_AMP || op == TK_NEWLINE) {
            if (check(p, TK_EOF) || check(p, TK_THEN) || check(p, TK_DO) ||
                check(p, TK_DONE) || check(p, TK_FI) || check(p, TK_ESAC) ||
                check(p, TK_ELSE) || check(p, TK_ELIF) || check(p, TK_RBRACE) ||
                check(p, TK_RPAREN) ||
                (check(p, TK_SEMI) && p->cur->val && strcmp(p->cur->val,";;") == 0)) {
                if (op == TK_AMP) {
                    node_t *n = node_new(N_LIST);
                    n->left = left; n->right = NULL; n->op = TK_AMP;
                    return n;
                }
                return left;
            }
            node_t *right = parse_and_or(p);
            if (!right) {
                if (op == TK_AMP) {
                    node_t *n = node_new(N_LIST);
                    n->left = left; n->right = NULL; n->op = TK_AMP;
                    return n;
                }
                return left;
            }
            node_t *n = node_new(N_LIST);
            n->left = left; n->right = right; n->op = op;
            left = n;
        }
    }
    return left;
}

static void resolve_heredocs(node_t *n, lexer_t *l) {
    if (!n) return;
    redir_t *r = NULL;
    switch (n->type) {
    case N_CMD:   r = n->redirs; break;
    case N_redir: r = n->redirs; resolve_heredocs(n->inner, l); break;
    case N_FUNC:  resolve_heredocs(n->func_body, l); r = n->redirs; break;
    default: break;
    }
    for (; r; r = r->next) {
        if (r->target && r->target[0] == '\x02') {
            int idx = atoi(r->target + 1);
            free(r->target);
            r->target = lexer_get_heredoc(l, idx);
        }
    }
    switch (n->type) {
    case N_PIPE:
        for (int i=0;i<n->ncmds;i++) resolve_heredocs(n->cmds[i],l); break;
    case N_LIST: case N_AND_OR:
        resolve_heredocs(n->left,l); resolve_heredocs(n->right,l); break;
    case N_IF:
        resolve_heredocs(n->cond,l); resolve_heredocs(n->body,l); resolve_heredocs(n->els,l); break;
    case N_WHILE: case N_UNTIL:
        resolve_heredocs(n->cond,l); resolve_heredocs(n->body,l); break;
    case N_FOR:
        resolve_heredocs(n->body,l); break;
    case N_CASE:
        for (node_t *item=n->cases;item;item=item->next_case) {
            resolve_heredocs(item->action,l);
        }
        break;
    case N_SUBSHELL: case N_BRACE: case N_BANG:
        resolve_heredocs(n->inner,l); break;
    default: break;
    }
}

node_t *parse(shell_t *sh, lexer_t *l) {
    if (setjmp(err_jmp)) return NULL;
    parser_t p = { sh, l, NULL, NULL };
    p.cur = lexer_next(l);
    skip_newlines(&p);
    if (check(&p, TK_EOF)) { token_free(p.cur); return NULL; }
    node_t *n = parse_list(&p);
    while (check(&p, TK_SEMI) || check(&p, TK_NEWLINE)) advance(&p);
    if (n) resolve_heredocs(n, l);
    token_free(p.cur);
    return n;
}

node_t *parse_string(shell_t *sh, const char *s) {
    lexer_t *l = lexer_new(sh, s, 0);
    node_t *n = parse(sh, l);
    lexer_free(l);
    return n;
}