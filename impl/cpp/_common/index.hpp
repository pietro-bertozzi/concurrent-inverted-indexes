// What the shared code needs from an index. A variant implements it with its own data
// structure and concurrency strategy; the harness (runner.cpp) never sees the difference.
#pragma once
#include <cstdint>
#include <functional>
#include <string>

#include "postings.hpp"

class Index {
public:
    virtual ~Index() = default;

    // Indexes document docId (one line of the corpus: tokens separated by single spaces).
    virtual void add(int docId, const std::string& doc) = 0;

    // Returns the posting list of term, sorted by document ID, or nullptr if the term is absent.
    virtual const PostingList* get(const std::string& term) const = 0;

    // Makes the index final (sorting, merging, publishing); timed as finalize_ns.
    virtual void finish() = 0;

    virtual int docs() const = 0;           // number of documents added
    virtual int terms() const = 0;          // number of distinct terms
    virtual int64_t postings() const = 0;   // total number of postings over all terms

    // Calls visit for every (term, posting list) pair, in any order; used by the dump.
    virtual void each(const std::function<void(const std::string&, const PostingList&)>& visit) const = 0;
};
