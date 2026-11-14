// Evaluates queries (docs/design.md, section Queries). The result of a query is the set of
// document IDs; the engine returns its size, which the caller adds up so that the work cannot
// be optimised away. AND and OR merge the two sorted lists with two pointers into a scratch
// vector that the engine keeps between calls (one engine per worker).
#pragma once
#include <cstdint>
#include <vector>

#include "index.hpp"
#include "queries.hpp"

class QueryEngine {
public:
    QueryEngine() : scratch_(1024) {}

    // Runs query number i of q on index and returns the number of matching documents.
    int run(const Index& index, const Queries& q, int i);

private:
    int intersect(const PostingList& a, const PostingList& b);
    int unite(const PostingList& a, const PostingList& b);
    void reserve(size_t n);

    std::vector<int32_t> scratch_;
};
