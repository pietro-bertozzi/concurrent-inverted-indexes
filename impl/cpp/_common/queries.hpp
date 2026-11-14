// The query file (docs/design.md, section Queries), parsed once: "SIMPLE t", "AND a b", "OR a b".
#pragma once
#include <string>
#include <vector>

struct Queries {
    enum Kind { SIMPLE = 0, AND = 1, OR = 2 };

    std::vector<int> kind;
    std::vector<std::string> first, second;   // second is empty for SIMPLE

    int size() const { return static_cast<int>(kind.size()); }
};

// Reads and parses file; throws std::runtime_error with the line number on a malformed line.
Queries loadQueries(const std::string& file);
