// The canonical dump (docs/design.md, section Correctness): terms sorted by their bytes (the
// order of `LC_ALL=C sort`: memcmp, a shorter term first if it is a prefix), one per line, then
// a tab and the postings "docid:frequency" sorted by document ID, separated by single spaces.
#pragma once
#include "index.h"

// Writes the index to file; ends the process if the file cannot be written.
void dump_write(const Index *index, const char *file);
