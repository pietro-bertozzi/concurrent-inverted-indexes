// The postings of one document: its distinct terms with their frequencies. This is the
// tokenizing job of the concurrent variants (docs/experiment-plan.md, section Each structural
// dimension): private to a worker, no shared data. One per worker, reused for every document.
//
// The terms are (pointer, length) pairs into the document line: nothing is copied. They are
// counted in a small open-addressing table (linear probing, FNV-1a), reset after every document.
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *term;    // points into the document line
    size_t length;
    uint64_t hash;       // FNV-1a 64-bit of the term, also used for the partition
    int32_t freq;
    size_t slot;         // its place in the table, to clear it after the document
} DocTerm;

typedef struct {
    DocTerm *terms;      // the distinct terms of the last document read, [0, count)
    int count;
    int capacity;
    int32_t *table;      // table[slot] = index in terms + 1, 0 = empty
    size_t table_capacity;   // power of two
} DocPostings;

// Creates an empty buffer.
void docpostings_init(DocPostings *d);

// Frees the buffer.
void docpostings_destroy(DocPostings *d);

// Replaces the contents with the postings of the document (doc, length).
void docpostings_read(DocPostings *d, const char *doc, size_t length);
