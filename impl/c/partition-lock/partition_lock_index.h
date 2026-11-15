// The C partition-lock index (docs/experiment-plan.md, How each variant works): the index is P
// TermMaps (our own hash map), a term lives in partition partition_of(term, P), and each
// partition has its own pthread mutex. A worker groups its document's postings by partition
// and takes each partition's lock once per `batch` postings (partitions in order 0 .. P-1). A
// query holds the locks of the partitions of its terms, the lower partition number first.
#pragma once
#include "args.h"
#include "index.h"

// Creates an empty index of args->partitions partitions, args->batch postings per acquisition.
Index *partition_lock_index_new(const Args *args);
