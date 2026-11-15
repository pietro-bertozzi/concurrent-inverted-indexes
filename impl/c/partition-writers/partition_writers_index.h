// The C partition-writers index (docs/experiment-plan.md, How each variant works): the index is P
// TermMaps (our own hash map), and every partition has exactly one owner, the only thread that
// ever writes it. The owners are the workers themselves: partition p belongs to worker p mod N.
// A worker groups its document's postings by partition into one private buffer per partition; a
// buffer that holds `batch` postings becomes a message. A message for a partition the worker owns
// is applied at once; any other goes into the owner's bounded queue (`queue` messages). A worker
// empties its own queue after every document and whenever it has to wait.
#pragma once
#include "args.h"
#include "index.h"

// Creates an empty index of args->partitions partitions with one queue of args->queue messages
// for each of the args->threads workers; args->batch postings per message.
Index *partition_writers_index_new(const Args *args);
