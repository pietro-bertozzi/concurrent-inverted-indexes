// A worker's private buffer of postings waiting to be inserted into the shared index
// (docs/experiment-plan.md, Rules every variant follows, Batching). Postings from several
// documents collect here; the variant flushes the buffer, under its lock, when it holds `batch`
// postings and at the end of every phase.
//
// A term is a (pointer, length) pair into its document line, which stays in memory until the
// repetition ends: nothing is copied (the map copies a term's bytes only when it is new).
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char **terms;
    size_t *lengths;
    int32_t *docs;
    int32_t *freqs;
    int size;
    int capacity;
} PostingBuffer;

// Creates an empty buffer with room for 64 postings.
void postingbuffer_init(PostingBuffer *b);

// Creates an empty buffer with room for `capacity` (at least 1) postings.
void postingbuffer_init_with(PostingBuffer *b, int capacity);

// Frees the buffer.
void postingbuffer_destroy(PostingBuffer *b);

// Appends one posting (term, document, frequency).
void postingbuffer_add(PostingBuffer *b, const char *term, size_t length, int doc_id, int freq);

// Empties the buffer (the arrays are kept for reuse).
static inline void postingbuffer_clear(PostingBuffer *b) { b->size = 0; }
