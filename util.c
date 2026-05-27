#include "oinit.h"
#include <stdarg.h>

void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

void *xmalloc(size_t n) {
    void *p = malloc(n);
    if (!p) die("out of memory");
    return p;
}

void *xcalloc(size_t n, size_t s) {
    void *p = calloc(n, s);
    if (!p) die("out of memory");
    return p;
}

void *xrealloc(void *p, size_t n) {
    p = realloc(p, n);
    if (!p) die("out of memory");
    return p;
}

char *xstrdup(const char *s) {
    char *r = strdup(s);
    if (!r) die("out of memory");
    return r;
}

char *xstrndup(const char *s, size_t n) {
    char *r = strndup(s, n);
    if (!r) die("out of memory");
    return r;
}

char *xasprintf(const char *fmt, ...) {
    va_list ap;
    char *p = NULL;
    va_start(ap, fmt);
    if (vasprintf(&p, fmt, ap) < 0) die("out of memory");
    va_end(ap);
    return p;
}

int xopen(const char *path, int flags, mode_t mode) {
    int fd = open(path, flags, mode);
    if (fd < 0) { perror(path); exit(1); }
    return fd;
}
