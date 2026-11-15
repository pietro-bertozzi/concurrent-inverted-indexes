#include "postingbuffer.h"

#include <stdlib.h>

#include "util.h"

void postingbuffer_init(PostingBuffer *b) {
    b->capacity = 64;
    b->size = 0;
    b->terms = xmalloc((size_t)b->capacity * sizeof(char *));
    b->lengths = xmalloc((size_t)b->capacity * sizeof(size_t));
    b->docs = xmalloc((size_t)b->capacity * sizeof(int32_t));
    b->freqs = xmalloc((size_t)b->capacity * sizeof(int32_t));
}

void postingbuffer_destroy(PostingBuffer *b) {
    free(b->terms);
    free(b->lengths);
    free(b->docs);
    free(b->freqs);
}

void postingbuffer_add(PostingBuffer *b, const char *term, size_t length, int doc_id, int freq) {
    if (b->size == b->capacity) {
        b->capacity *= 2;
        b->terms = xrealloc(b->terms, (size_t)b->capacity * sizeof(char *));
        b->lengths = xrealloc(b->lengths, (size_t)b->capacity * sizeof(size_t));
        b->docs = xrealloc(b->docs, (size_t)b->capacity * sizeof(int32_t));
        b->freqs = xrealloc(b->freqs, (size_t)b->capacity * sizeof(int32_t));
    }
    b->terms[b->size] = term;
    b->lengths[b->size] = length;
    b->docs[b->size] = (int32_t)doc_id;
    b->freqs[b->size] = (int32_t)freq;
    b->size++;
}
