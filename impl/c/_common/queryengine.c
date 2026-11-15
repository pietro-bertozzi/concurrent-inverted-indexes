#include "queryengine.h"

#include <stdlib.h>

#include "util.h"

static const PostingList EMPTY = {NULL, NULL, 0, 0};

// The posting list of term, or the empty list if the term is absent.
static const PostingList *list_of(const Index *index, const char *term, size_t length) {
    const PostingList *l = index->get(index, term, length);
    return l == NULL ? &EMPTY : l;
}

// Makes sure the scratch array can hold n IDs.
static void reserve(QueryEngine *e, size_t n) {
    if (e->capacity >= n) return;
    size_t c = e->capacity * 2 > n ? e->capacity * 2 : n;
    e->scratch = xrealloc(e->scratch, c * sizeof(int32_t));
    e->capacity = c;
}

// Intersection of two sorted lists: advance the pointer with the smaller ID, keep equal IDs.
static int intersect(QueryEngine *e, const PostingList *a, const PostingList *b) {
    reserve(e, a->size < b->size ? a->size : b->size);
    size_t i = 0, j = 0, n = 0;
    while (i < a->size && j < b->size) {
        int32_t x = a->docs[i], y = b->docs[j];
        if (x == y) { e->scratch[n++] = x; i++; j++; }
        else if (x < y) i++;
        else j++;
    }
    return (int)n;
}

// Union of two sorted lists: always take the smaller ID, an ID present in both is taken once.
static int unite(QueryEngine *e, const PostingList *a, const PostingList *b) {
    reserve(e, a->size + b->size);
    size_t i = 0, j = 0, n = 0;
    while (i < a->size && j < b->size) {
        int32_t x = a->docs[i], y = b->docs[j];
        if (x == y) { e->scratch[n++] = x; i++; j++; }
        else if (x < y) { e->scratch[n++] = x; i++; }
        else { e->scratch[n++] = y; j++; }
    }
    while (i < a->size) e->scratch[n++] = a->docs[i++];
    while (j < b->size) e->scratch[n++] = b->docs[j++];
    return (int)n;
}

void queryengine_init(QueryEngine *e) {
    e->capacity = 1024;
    e->scratch = xmalloc(e->capacity * sizeof(int32_t));
}

void queryengine_destroy(QueryEngine *e) {
    free(e->scratch);
    e->scratch = NULL;
    e->capacity = 0;
}

int queryengine_run(QueryEngine *e, const Index *index, const Queries *q, int i) {
    const PostingList *a = list_of(index, q->first[i], q->first_length[i]);
    if (q->kind[i] == Q_SIMPLE) return (int)a->size;
    const PostingList *b = list_of(index, q->second[i], q->second_length[i]);
    return q->kind[i] == Q_AND ? intersect(e, a, b) : unite(e, a, b);
}
