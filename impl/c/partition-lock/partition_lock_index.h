// The C partition-lock index (docs/experiment-plan.md, How each variant works): the index is P
// TermMaps (our own hash map), a term lives in partition partition_of(term, P), and each
// partition has its own pthread mutex. A worker groups its document's postings by partition into
// one private buffer per partition; when a buffer holds `batch` postings (from one or several
// documents) it takes that partition's lock and inserts them all, and it flushes what is left at
// the end of every phase. A query holds the locks of the partitions of its terms, the lower
// partition number first.
#pragma once
#include "args.h"
#include "index.h"

// Creates an empty index of args->partitions partitions, args->batch postings per acquisition.
Index *partition_lock_index_new(const Args *args);
