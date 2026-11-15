// What the shared code needs from an index. C has no classes, so an index is a struct of
// function pointers (the same role as the abstract class in C++ and the interface in Java and
// Go); a variant embeds it as the first member of its own struct and fills it in. The harness
// (runner.c) never sees the difference.
//
// Every worker thread gets its own Worker from index->worker(); all the operations that run
// concurrently go through it. The other functions are called by one thread only, outside the
// concurrent phases (or, for get, by a worker that already holds the locks its variant needs).
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "postings.h"
#include "queries.h"

typedef struct Index Index;
typedef struct Worker Worker;

// Callback of each(): one (term, posting list) pair; the term is not NUL-terminated.
typedef void (*IndexVisitor)(void *context, const char *term, size_t length, const PostingList *list);

// The per-worker handle: private buffers live here, so workers never share them. A variant
// embeds it as the first member of its own handle struct.
struct Worker {
    // Indexes document doc_id (one line of the corpus: tokens separated by single spaces).
    void (*add)(Worker *self, int doc_id, const char *doc, size_t length);
    // Runs query number i of q and returns the number of matching documents.
    int (*query)(Worker *self, const Queries *q, int i);
    // Inserts every posting this worker still holds in a private buffer. Called by the worker's
    // own thread at the end of every phase (the indexing loop, the timed query or mixed phase).
    void (*flush)(Worker *self);
};

struct Index {
    // Makes a handle for one worker thread; called before timing starts. The index owns it
    // and frees it in destroy.
    Worker *(*worker)(Index *self);
    // Returns the posting list of term (sorted by document ID), or NULL if the term is absent.
    // No synchronisation of its own: a concurrent variant calls it with the right locks held.
    const PostingList *(*get)(const Index *self, const char *term, size_t length);
    // Makes the index final (sorting, merging, publishing); timed as finalize_ns.
    void (*finish)(Index *self);
    int (*docs)(const Index *self);           // number of documents added
    int (*terms)(const Index *self);          // number of distinct terms
    int64_t (*postings)(const Index *self);   // total number of postings over all terms
    // Calls visit for every (term, posting list) pair, in any order; used by the dump.
    void (*each)(const Index *self, IndexVisitor visit, void *context);
    // Frees the index and everything it owns, its worker handles included.
    void (*destroy)(Index *self);
};
