// The worker loop shared by every variant (docs/experiment-plan.md, Rules every variant
// follows): N threads run the same body, and documents are handed out dynamically from a
// shared counter. A worker that finishes a document takes the next one, so faster cores
// simply take more documents.
#pragma once
#include <stddef.h>

#include "index.h"

// The body of a worker: called once on every thread with the shared context and the worker's number.
typedef void (*WorkerBody)(void *context, int worker);

// Runs body(context, w) for w = 0 .. n-1, each on its own pthread, and waits for all of them.
void workers_run(int n, WorkerBody body, void *context);

// Indexes the documents from (inclusive) to to (exclusive), given as lines (pointer, length):
// each worker repeatedly takes the next document ID from a shared atomic counter
// (d = counter++) and adds it. The counter is touched once per document, not once per word,
// so it costs almost nothing.
void workers_index(Worker **workers, int n, char *const *line, const size_t *length, int from, int to);
