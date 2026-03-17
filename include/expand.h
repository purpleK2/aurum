#ifndef EXPAND_H
#define EXPAND_H

#include <aurum.h>

char **expand_words(shell_t *sh, char **words, int n, int *outc);
char *expand_word(shell_t *sh, const char *w);
char *expand_param(shell_t *sh, const char *w);
int pattern_match(const char *pat, const char *str);
char **glob_expand(shell_t *sh, const char *pat, int *n);

#endif /* EXPAND_H */