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

    // Inserts a posting in its correct place, keeping the list sorted (docs/design.md, section
    // Concurrent variants). With dynamic handout documents arrive nearly in order, so the place
    // is almost always the end or a few slots before it. The document ID must not be in the
    // list yet.
    void insert(int docId, int freq) {
        size_t pos = docs.size();
        while (pos > 0 && docs[pos - 1] > docId) pos--;   // walk back from the end
        docs.insert(docs.begin() + static_cast<std::ptrdiff_t>(pos), static_cast<int32_t>(docId));
        freqs.insert(freqs.begin() + static_cast<std::ptrdiff_t>(pos), static_cast<int32_t>(freq));
    }
};
