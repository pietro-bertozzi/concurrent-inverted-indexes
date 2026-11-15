#include "util.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

void fatal(const char *format, ...) {
    va_list args;
    va_start(args, format);
    fputs("error: ", stderr);
    vfprintf(stderr, format, args);
    fputc('\n', stderr);
    va_end(args);
    exit(1);
}

void *xmalloc(size_t size) {
    void *p = malloc(size == 0 ? 1 : size);
    if (p == NULL) fatal("out of memory (%zu bytes)", size);
    return p;
}

void *xcalloc(size_t count, size_t size) {
    void *p = calloc(count == 0 ? 1 : count, size == 0 ? 1 : size);
    if (p == NULL) fatal("out of memory (%zu x %zu bytes)", count, size);
    return p;
}

void *xrealloc(void *p, size_t size) {
    void *q = realloc(p, size == 0 ? 1 : size);
    if (q == NULL) fatal("out of memory (%zu bytes)", size);
    return q;
}
