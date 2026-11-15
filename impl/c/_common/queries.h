// The query file (docs/design.md, section Queries), parsed once: "SIMPLE t", "AND a b", "OR a b".
#pragma once
#include <stddef.h>

enum { Q_SIMPLE = 0, Q_AND = 1, Q_OR = 2 };

typedef struct {
    int count;
    int *kind;
    char **first;          // first term of each query (NUL-terminated)
    size_t *first_length;
    char **second;         // second term; NULL for SIMPLE
    size_t *second_length;
} Queries;

// Reads and parses file; ends the process with the line number on a malformed line.
Queries queries_load(const char *file);

// Frees everything queries_load allocated.
void queries_free(Queries *q);
