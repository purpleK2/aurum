#ifndef TOKEN_H
#define TOKEN_H

typedef enum {
    TK_WORD, TK_ASSIGN,
    TK_PIPE,            // |
    TK_AND,             // &&
    TK_OR,              // ||
    TK_SEMI,            // ;
    TK_AMP,             // &
    TK_NEWLINE,
    TK_redir_IN,        // <
    TK_redir_OUT,       // >
    TK_redir_APP,       // >>
    TK_redir_HER,       // <<
    TK_redir_HED,       // <<-
    TK_redir_DUP_IN,    // <&
    TK_redir_DUP_OUT,   // >&
    TK_redir_CLOBBER,   // >|
    TK_LPAREN,          // (
    TK_RPAREN,          // )
    TK_LBRACE,          // {
    TK_RBRACE,          // }
    TK_BANG,            // !
    TK_IF, TK_THEN, TK_ELSE, TK_ELIF, TK_FI,
    TK_WHILE, TK_UNTIL, TK_DO, TK_DONE,
    TK_FOR, TK_IN,
    TK_CASE, TK_ESAC,
    TK_FUNCTION,
    TK_EOF
} token_type;

typedef struct token {
    token_type type;
    char *val;
    int lineno;
} token_t;

#endif /* TOKEN_H */