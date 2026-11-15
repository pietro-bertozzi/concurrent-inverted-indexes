// The sequential index: one std::unordered_map from term to posting list, no synchronisation.
// Documents are added in increasing ID order, so every posting list is already sorted and
// finish() has nothing to do.
#pragma once
#include <string>
#include <unordered_map>

#include "index.hpp"
#include "queryengine.hpp"
#include "tokenizer.hpp"

class SequentialIndex : public Index, public Worker {
public:
    // Only one worker exists: the index is its own handle.
    Worker* worker() override { return this; }

    int query(const Queries& q, int i) override { return engine_.run(*this, q, i); }

    void add(int docId, const std::string& doc) override {
        Tokenizer tokens(doc);
        while (tokens.next()) {
            // C++17 cannot look up an unordered_map<std::string, ...> by string_view, so every
            // token is copied into a std::string first (docs/design.md, Conventions, C++).
            // try_emplace moves that string into the map only if the term is new.
            auto [it, inserted] = map_.try_emplace(std::string(tokens.token()));
            PostingList& list = it->second;
            // A term seen earlier in this document is the last posting of its list: count it.
            if (list.size() > 0 && list.docs.back() == docId) {
                list.freqs.back()++;
            } else {
                list.append(docId, 1);
                postings_++;
            }
        }
        docs_++;
    }

    const PostingList* get(const std::string& term) const override {
        auto it = map_.find(term);
        return it == map_.end() ? nullptr : &it->second;
    }

    void finish() override { /* sorted by construction */ }
    int docs() const override { return docs_; }
    int terms() const override { return static_cast<int>(map_.size()); }
    int64_t postings() const override { return postings_; }

    void each(const std::function<void(const std::string&, const PostingList&)>& visit) const override {
        for (const auto& [term, list] : map_) visit(term, list);
    }

private:
    std::unordered_map<std::string, PostingList> map_;
    int docs_ = 0;
    int64_t postings_ = 0;
    QueryEngine engine_;
};
