// The C private-merge index (docs/experiment-plan.md, How each variant works). While building,
// every worker writes only into its own private index (N TermMaps, one pile per partition
// number, P = N workers), with no lock. In finish() thread i merges pile i of every worker, so
// all workers merge at once, each on different terms, with no lock either. A worker takes
// increasing document IDs, so each private list is already sorted and the merge of a term is an
// N-way merge in document order (a linear scan of the heads or, in private-merge-heap, a
// binary min-heap). Queries (query mode, after finish) read the merged piles with no lock.
#pragma once
#include "args.h"
#include "index.h"

// Creates an empty index for args->threads workers (documents handed out dynamically).
Index *private_merge_index_new(const Args *args);

// The same index, with the documents split into args->threads equal ranges up front
// (private-merge-static): only the handout differs.
Index *private_merge_static_index_new(const Args *args);

// The same index with the N-way merge of a term done with a binary min-heap of the list heads
// (private-merge-heap): only the merge differs.
Index *private_merge_heap_index_new(const Args *args);
