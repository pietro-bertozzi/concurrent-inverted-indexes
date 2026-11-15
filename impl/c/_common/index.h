// What the shared code needs from an index. C has no classes, so an index is a struct of
// function pointers (the same role as the abstract class in C++ and the interface in Java and
// Go); a variant embeds it as the first member of its own struct and fills it in. The harness
// (runner.c) never sees the difference.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "postings.h"

typedef struct Index Index;

// Callback of each(): one (term, posting list) pair; the term is not NUL-terminated.
typedef void (*IndexVisitor)(void *context, const char *term, size_t length, const PostingList *list);

struct Index {
    // Indexes document doc_id (one line of the corpus: tokens separated by single spaces).
    void (*add)(Index *self, int doc_id, const char *doc, size_t length);
    // Returns the posting list of term (sorted by document ID), or NULL if the term is absent.
    const PostingList *(*get)(const Index *self, const char *term, size_t length);
    // Makes the index final (sorting, merging, publishing); timed as finalize_ns.
    void (*finish)(Index *self);
    int (*docs)(const Index *self);           // number of documents added
    int (*terms)(const Index *self);          // number of distinct terms
    int64_t (*postings)(const Index *self);   // total number of postings over all terms
    // Calls visit for every (term, posting list) pair, in any order; used by the dump.
    void (*each)(const Index *self, IndexVisitor visit, void *context);
    // Frees the index and everything it owns.
    void (*destroy)(Index *self);
};
