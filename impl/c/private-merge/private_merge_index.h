// The C private-merge index (docs/experiment-plan.md, How each variant works). While building,
// every worker writes only into its own private index (N TermMaps, one pile per partition
// number, P = N workers), with no lock. In finish() thread i merges pile i of every worker, so
// all workers merge at once, each on different terms, with no lock either. A worker takes
// increasing document IDs, so each private list is already sorted and the merge of a term is an
// N-way merge in document order. Queries (query mode, after finish) read the merged piles with no lock.
#pragma once
#include "args.h"
#include "index.h"

// Creates an empty index for args->threads workers.
Index *private_merge_index_new(const Args *args);
