// A worker's private buffer of postings waiting to be inserted into the shared index
// (docs/experiment-plan.md, Rules every variant follows, Batching). Postings from several
// documents collect here; the variant flushes the buffer, under its lock, when it holds `batch`
// postings and at the end of every phase.
//
// The buffer owns a copy of every term (a std::string): the pointers of DocPostings point into
// a map that is emptied for the next document. The flush moves the strings into the index map,
// so a term that is new costs no second copy.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct PostingBuffer {
    std::vector<std::string> terms;
    std::vector<int32_t> docs;
    std::vector<int32_t> freqs;

    size_t size() const { return terms.size(); }

    // Appends one posting (term, document, frequency).
    void add(const std::string& term, int docId, int freq) {
        terms.push_back(term);
        docs.push_back(static_cast<int32_t>(docId));
        freqs.push_back(static_cast<int32_t>(freq));
    }

    // Empties the buffer (the vectors keep their capacity for reuse).
    void clear() {
        terms.clear();
        docs.clear();
        freqs.clear();
    }
};
