// A posting list: a growable array of (document ID, term frequency) pairs, kept as two
// int32 vectors (std::vector doubles its capacity when full).
#pragma once
#include <cstdint>
#include <vector>

struct PostingList {
    std::vector<int32_t> docs;
    std::vector<int32_t> freqs;

    size_t size() const { return docs.size(); }

    // Appends a posting; the caller appends in increasing document ID order.
    void append(int docId, int freq) {
        docs.push_back(static_cast<int32_t>(docId));
        freqs.push_back(static_cast<int32_t>(freq));
    }
};
