// A posting list: a growable array of (document ID, term frequency) pairs, kept as two int32
// arrays. The capacity starts at 2 and doubles when full, like Java's (docs/design.md).
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int32_t *docs;
    int32_t *freqs;
    size_t size;
    size_t capacity;
} PostingList;

// Allocates an empty posting list (no arrays yet; the first append allocates them).
PostingList *postings_new(void);

// Frees the list and its arrays.
void postings_free(PostingList *list);

// Appends a posting; the caller appends in increasing document ID order.
void postings_append(PostingList *list, int doc_id, int freq);

// Inserts a posting in its correct place, keeping the list sorted (docs/design.md, section
// Concurrent variants). With dynamic handout documents arrive nearly in order, so the place is
// almost always the end (a plain append) or a few slots before it. The document ID must not
// be in the list yet.
void postings_insert(PostingList *list, int doc_id, int freq);
