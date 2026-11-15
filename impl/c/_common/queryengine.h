// Evaluates queries (docs/design.md, section Queries). The result of a query is the set of
// document IDs; the engine returns its size, which the caller adds up so that the work cannot
// be optimised away. AND and OR merge the two sorted lists with two pointers into a scratch
// array that the engine keeps between calls (one engine per worker).
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "index.h"
#include "queries.h"

typedef struct {
    int32_t *scratch;
    size_t capacity;
} QueryEngine;

// Creates an engine with its scratch array.
void queryengine_init(QueryEngine *e);

// Frees the scratch array.
void queryengine_destroy(QueryEngine *e);

// Runs query number i of q on index and returns the number of matching documents.
int queryengine_run(QueryEngine *e, const Index *index, const Queries *q, int i);
