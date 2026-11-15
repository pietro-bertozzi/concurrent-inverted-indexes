// The C global-lock index (docs/experiment-plan.md, How each variant works): one TermMap (our
// own hash map) and one pthread mutex for the whole index. A worker tokenizes its document
// without the lock and collects the postings in a private buffer; when the buffer holds
// `batch` postings (from one or several documents) it takes the lock and inserts them all, and
// it flushes what is left at the end of every phase. A query holds the same lock while it
// reads and merges its lists.
#pragma once
#include "args.h"
#include "index.h"

// Creates an empty index that inserts args->batch postings per lock acquisition.
Index *global_lock_index_new(const Args *args);
