// The postings of one document: its distinct terms with their frequencies. This is the
// tokenizing job of the concurrent variants (docs/experiment-plan.md, section Each structural
// dimension): private to a worker, no shared data. One per worker, reused for every document.
#pragma once
#include <string>
#include <unordered_map>
#include <vector>

#include "tokenizer.hpp"

class DocPostings {
public:
    // The distinct terms of the last document read and their frequencies, [0, size()).
    // The term pointers point into the counting map and stay valid until the next read().
    std::vector<const std::string*> terms;
    std::vector<int> freqs;

    size_t size() const { return terms.size(); }

    // Replaces the contents with the postings of doc.
    void read(const std::string& doc) {
        counts_.clear();
        Tokenizer tokens(doc);
        // As in the sequential variant, every token is copied into a std::string first (C++17
        // cannot look up an unordered_map<std::string, ...> by string_view).
        while (tokens.next()) counts_[std::string(tokens.token())]++;
        terms.clear();
        freqs.clear();
        for (const auto& [term, freq] : counts_) {
            terms.push_back(&term);
            freqs.push_back(freq);
        }
    }

private:
    std::unordered_map<std::string, int> counts_;
};
