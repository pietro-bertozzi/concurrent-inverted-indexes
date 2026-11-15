#define _POSIX_C_SOURCE 200809L
#include "queries.h"

#include <stdio.h>
#include <sys/types.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

// Copies length bytes into a new NUL-terminated string.
static char *copy(const char *s, size_t length) {
    char *c = xmalloc(length + 1);
    memcpy(c, s, length);
    c[length] = '\0';
    return c;
}

Queries queries_load(const char *file) {
    FILE *in = fopen(file, "rb");
    if (in == NULL) fatal("cannot open %s", file);
    Queries q;
    memset(&q, 0, sizeof q);
    size_t capacity = 0;
    char *line = NULL;
    size_t line_capacity = 0;
    ssize_t n;
    int number = 0;
    while ((n = getline(&line, &line_capacity, in)) >= 0) {
        number++;
        if (n > 0 && line[n - 1] == '\n') n--;
        if (n > 0 && line[n - 1] == '\r') n--;
        line[n] = '\0';
        // Split at single spaces into at most 4 fields (a 4th one makes the line invalid).
        const char *f[4];
        size_t fl[4];
        int fields = 0;
        size_t start = 0;
        for (;;) {
            const char *space = memchr(line + start, ' ', (size_t)n - start);
            size_t end = space == NULL ? (size_t)n : (size_t)(space - line);
            if (fields < 4) { f[fields] = line + start; fl[fields] = end - start; }
            fields++;
            if (space == NULL) break;
            start = end + 1;
        }
        int kind;
        if (fl[0] == 6 && memcmp(f[0], "SIMPLE", 6) == 0) kind = Q_SIMPLE;
        else if (fl[0] == 3 && memcmp(f[0], "AND", 3) == 0) kind = Q_AND;
        else if (fl[0] == 2 && memcmp(f[0], "OR", 2) == 0) kind = Q_OR;
        else fatal("%s line %d: unknown operator '%.*s'", file, number, (int)fl[0], f[0]);
        if (fields != (kind == Q_SIMPLE ? 2 : 3)) fatal("%s line %d: wrong number of terms for %.*s", file, number, (int)fl[0], f[0]);
        if ((size_t)q.count == capacity) {
            capacity = capacity == 0 ? 1024 : capacity * 2;
            q.kind = xrealloc(q.kind, capacity * sizeof(int));
            q.first = xrealloc(q.first, capacity * sizeof(char *));
            q.first_length = xrealloc(q.first_length, capacity * sizeof(size_t));
            q.second = xrealloc(q.second, capacity * sizeof(char *));
            q.second_length = xrealloc(q.second_length, capacity * sizeof(size_t));
        }
        q.kind[q.count] = kind;
        q.first[q.count] = copy(f[1], fl[1]);
        q.first_length[q.count] = fl[1];
        q.second[q.count] = kind == Q_SIMPLE ? NULL : copy(f[2], fl[2]);
        q.second_length[q.count] = kind == Q_SIMPLE ? 0 : fl[2];
        q.count++;
    }
    free(line);
    fclose(in);
    if (q.count == 0) fatal("%s has no queries", file);
    return q;
}

void queries_free(Queries *q) {
    for (int i = 0; i < q->count; i++) {
        free(q->first[i]);
        free(q->second[i]);
    }
    free(q->kind);
    free(q->first);
    free(q->first_length);
    free(q->second);
    free(q->second_length);
    memset(q, 0, sizeof *q);
}
