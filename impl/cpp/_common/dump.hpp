// The canonical dump (docs/design.md, section Correctness): terms sorted by their bytes (the
// order of `LC_ALL=C sort`; std::string compares unsigned bytes), one per line, then a tab and
// the postings "docid:frequency" sorted by document ID, separated by single spaces.
#pragma once
#include <string>

#include "index.hpp"

// Writes the index to file; throws std::runtime_error if the file cannot be written.
void writeDump(const Index& index, const std::string& file);
