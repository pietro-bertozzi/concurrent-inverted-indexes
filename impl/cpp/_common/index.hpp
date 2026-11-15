// What the shared code needs from an index. A variant implements it with its own data
// structure and concurrency strategy; the harness (runner.cpp) never sees the difference.
//
// Every worker thread gets its own Worker from Index::worker(); all the operations that run
// concurrently go through it. The other methods are called by one thread only, outside the
// concurrent phases (or, for get, by a worker that already holds the locks its variant needs).
#pragma once
#include <cstdint>
#include <functional>
#include <string>

#include "postings.hpp"
#include "queries.hpp"

// The per-worker handle: private buffers live here, so workers never share them.
class Worker {
public:
    virtual ~Worker() = default;

    // Indexes document docId (one line of the corpus: tokens separated by single spaces).
    virtual void add(int docId, const std::string& doc) = 0;

    // Runs query number i of q and returns the number of matching documents.
    virtual int query(const Queries& q, int i) = 0;
};

class Index {
public:
    virtual ~Index() = default;

    // Makes a handle for one worker thread; called before timing starts. The index owns it.
    virtual Worker* worker() = 0;

    // Returns the posting list of term, sorted by document ID, or nullptr if the term is absent.
    // No synchronisation of its own: a concurrent variant calls it with the right locks held.
    virtual const PostingList* get(const std::string& term) const = 0;

    // Makes the index final (sorting, merging, publishing); timed as finalize_ns.
    virtual void finish() = 0;

    virtual int docs() const = 0;           // number of documents added
    virtual int terms() const = 0;          // number of distinct terms
    virtual int64_t postings() const = 0;   // total number of postings over all terms

    // Calls visit for every (term, posting list) pair, in any order; used by the dump.
    virtual void each(const std::function<void(const std::string&, const PostingList&)>& visit) const = 0;
};
