#include "aurum.h"

#include "util.h"
#include "string.h"
#ifdef USE_LINEEDITOR
#include "termios.h"
#endif
#include "unistd.h"
#include "stdlib.h"

void history_add(shell_t *sh, const char *line) {
    if (!line || !*line) return;
    if (sh->hist_size > 0 &&
        strcmp(sh->history[sh->hist_size - 1], line) == 0) return;
    if (sh->hist_size >= sh->hist_cap) {
        sh->hist_cap = sh->hist_cap ? sh->hist_cap * 2 : 64;
        sh->history  = sh_realloc(sh->history,
                                  sh->hist_cap * sizeof(char *));
    }
    sh->history[sh->hist_size++] = sh_strdup(line);
}

#ifdef USE_LINEEDITOR

static struct termios orig_termios;
static int raw_active = 0;

static void term_raw(void) {
    struct termios raw;
    tcgetattr(0, &orig_termios);
    raw = orig_termios;
    raw.c_iflag &= ~(unsigned)(ICRNL | IXON);
    raw.c_lflag &= ~(unsigned)(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN]  = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(0, TCSAFLUSH, &raw);
    raw_active = 1;
}

static void term_restore(void) {
    if (raw_active) {
        tcsetattr(0, TCSAFLUSH, &orig_termios);
        raw_active = 0;
    }
}

static void refresh_line(const char *prompt, const char *buf,
                          int len, int pos) {
    write(1, "\r", 1);
    write(1, prompt, strlen(prompt));
    write(1, buf, (size_t)len);
    write(1, "\x1b[K", 3);
    int back = len - pos;
    if (back > 0) {
        char esc[32];
        int n = snprintf(esc, sizeof esc, "\x1b[%dD", back);
        write(1, esc, (size_t)n);
    }
}

char *read_line(shell_t *sh, const char *prompt) {
    if (!isatty(0)) {
        if (prompt) { fputs(prompt, stderr); fflush(stderr); }
        char *line = NULL;
        size_t sz  = 0;
        ssize_t n  = getline(&line, &sz, stdin);
        if (n < 0) { free(line); return NULL; }
        if (n > 0 && line[n - 1] == '\n') line[n - 1] = '\0';
        char *copy = sh_strdup(line);
        free(line);
        return copy;
    }

    if (prompt) { fputs(prompt, stderr); fflush(stderr); }
    term_raw();

    int    bufcap     = 256;
    char  *buf        = sh_malloc(bufcap);
    int    len        = 0, pos = 0;
    int    hist_idx   = sh->hist_size;
    char  *saved_line = NULL;

    write(1, "\r", 1);
    if (prompt) write(1, prompt, strlen(prompt));

    for (;;) {
        unsigned char c;
        if (read(0, &c, 1) <= 0) {
            term_restore();
            if (len == 0) { free(buf); free(saved_line); return NULL; }
            break;
        }

        if (c == '\r' || c == '\n') { write(1, "\r\n", 2); break; }

        if (c == 3) {
            write(1, "^C\r\n", 4);
            buf[0] = '\0'; len = 0; pos = 0;
            break;
        }
        if (c == 4) {
            if (len == 0) {
                write(1, "\r\n", 2);
                term_restore(); free(buf); free(saved_line);
                return NULL;
            }
            if (pos < len) {
                memmove(buf + pos, buf + pos + 1, (size_t)(len - pos - 1));
                len--;
            }
        } else if (c == 127 || c == 8) { 
            if (pos > 0) {
                memmove(buf + pos - 1, buf + pos, (size_t)(len - pos));
                len--; pos--;
            }
        } else if (c == 1)  { pos = 0;   }
        else if  (c == 5)  { pos = len;  }
        else if  (c == 11) { len = pos;  }
        else if  (c == 21) {
            memmove(buf, buf + pos, (size_t)(len - pos));
            len -= pos; pos = 0;
        } else if (c == 23) {
            int i = pos;
            while (i > 0 && buf[i-1] == ' ') i--;
            while (i > 0 && buf[i-1] != ' ') i--;
            memmove(buf + i, buf + pos, (size_t)(len - pos));
            len -= (pos - i); pos = i;
        } else if (c == 27) {
            unsigned char seq[4] = {0};
            if (read(0, &seq[0], 1) != 1) goto redraw;
            if (seq[0] == '[') {
                if (read(0, &seq[1], 1) != 1) goto redraw;
                if (seq[1] >= '0' && seq[1] <= '9') {
                    read(0, &seq[2], 1);
                    if (seq[1] == '3' && seq[2] == '~' && pos < len) {
                        memmove(buf+pos, buf+pos+1, (size_t)(len-pos-1));
                        len--;
                    }
                } else {
                    switch (seq[1]) {
                    case 'A':
                        if (hist_idx > 0) {
                            if (hist_idx == sh->hist_size) {
                                free(saved_line);
                                saved_line = sh_strndup(buf, len);
                            }
                            hist_idx--;
                            const char *hl = sh->history[hist_idx];
                            int hl_len = (int)strlen(hl);
                            if (hl_len >= bufcap) {
                                bufcap = hl_len + 64;
                                buf = sh_realloc(buf, bufcap);
                            }
                            memcpy(buf, hl, (size_t)hl_len);
                            len = hl_len; pos = len;
                        }
                        break;
                    case 'B':
                        if (hist_idx < sh->hist_size) {
                            hist_idx++;
                            const char *hl =
                                hist_idx == sh->hist_size
                                ? (saved_line ? saved_line : "")
                                : sh->history[hist_idx];
                            int hl_len = (int)strlen(hl);
                            if (hl_len >= bufcap) {
                                bufcap = hl_len + 64;
                                buf = sh_realloc(buf, bufcap);
                            }
                            memcpy(buf, hl, (size_t)hl_len);
                            len = hl_len; pos = len;
                        }
                        break;
                    case 'C': if (pos < len) pos++; break;
                    case 'D': if (pos > 0)   pos--; break;
                    case 'H': pos = 0;   break;
                    case 'F': pos = len; break;
                    }
                }
            } else if (seq[0] == 'O') {
                unsigned char ch2;
                if (read(0, &ch2, 1) == 1) {
                    if (ch2 == 'H') pos = 0;
                    if (ch2 == 'F') pos = len;
                }
            }
        } else if (c >= 32) {
            if (len + 1 >= bufcap) { bufcap *= 2; buf = sh_realloc(buf, bufcap); }
            memmove(buf + pos + 1, buf + pos, (size_t)(len - pos));
            buf[pos] = (char)c;
            len++; pos++;
        }

    redraw:
        buf[len] = '\0';
        refresh_line(prompt ? prompt : "", buf, len, pos);
    }

    term_restore();
    buf[len] = '\0';
    free(saved_line);
    return buf;
}

#else

char *read_line(shell_t *sh, const char *prompt) {
    (void)sh;
    if (prompt) { fputs(prompt, stderr); fflush(stderr); }
    char *line = NULL;
    size_t sz  = 0;
    ssize_t n  = getline(&line, &sz, stdin);
    if (n < 0) { free(line); return NULL; }
    if (n > 0 && line[n - 1] == '\n') line[n - 1] = '\0';
    char *copy = sh_strdup(line);
    free(line);
    return copy;
}

#endif /* USE_LINEEDITOR */