#ifndef UTIL_H
#define UTIL_H

#include <aurum.h>

#include <stddef.h>
#include <stdarg.h>

void sh_error(shell_t *sh, const char *fmt, ...);
void sh_warn(shell_t *sh, const char *fmt, ...);
char *sh_strdup(const char *s);
char *sh_strndup(const char *s, size_t n);
char *sh_asprintf(const char *fmt, ...);
void *sh_malloc(size_t n);
void *sh_realloc(void *p, size_t n);
int is_name(const char *s);
int is_special_param(char c);
char *join_words(char **words, int n, const char *sep);

#endif /* UTIL_H */