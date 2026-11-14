#include "queryengine.hpp"

#include <algorithm>

namespace {
const PostingList EMPTY;

const PostingList& list(const Index& index, const std::string& term) {
    const PostingList* l = index.get(term);
    return l == nullptr ? EMPTY : *l;
}
}  // namespace

int QueryEngine::run(const Index& index, const Queries& q, int i) {
    const PostingList& a = list(index, q.first[i]);
    if (q.kind[i] == Queries::SIMPLE) return static_cast<int>(a.size());
    const PostingList& b = list(index, q.second[i]);
    return q.kind[i] == Queries::AND ? intersect(a, b) : unite(a, b);
}

// Intersection of two sorted lists: advance the pointer with the smaller ID, keep equal IDs.
int QueryEngine::intersect(const PostingList& a, const PostingList& b) {
    reserve(std::min(a.size(), b.size()));
    size_t i = 0, j = 0, n = 0;
    while (i < a.size() && j < b.size()) {
        int32_t x = a.docs[i], y = b.docs[j];
        if (x == y) { scratch_[n++] = x; i++; j++; }
        else if (x < y) i++;
        else j++;
    }
    return static_cast<int>(n);
}

// Union of two sorted lists: always take the smaller ID, an ID present in both is taken once.
int QueryEngine::unite(const PostingList& a, const PostingList& b) {
    reserve(a.size() + b.size());
    size_t i = 0, j = 0, n = 0;
    while (i < a.size() && j < b.size()) {
        int32_t x = a.docs[i], y = b.docs[j];
        if (x == y) { scratch_[n++] = x; i++; j++; }
        else if (x < y) { scratch_[n++] = x; i++; }
        else { scratch_[n++] = y; j++; }
    }
    while (i < a.size()) scratch_[n++] = a.docs[i++];
    while (j < b.size()) scratch_[n++] = b.docs[j++];
    return static_cast<int>(n);
}

// Makes sure the scratch vector can hold n IDs.
void QueryEngine::reserve(size_t n) {
    if (scratch_.size() < n) scratch_.resize(std::max(n, scratch_.size() * 2));
}
