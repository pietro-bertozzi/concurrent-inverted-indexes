// The private-merge index (docs/experiment-plan.md, How each variant works). While building,
// every worker writes only into its own private index, with no lock: its terms are already
// divided into N piles by the partition hash (P = N, the number of workers). In finish()
// worker i merges pile i of every worker, so all workers merge at once, each on different terms,
// with no lock either. A worker takes increasing document IDs, so each private list is already
// sorted and the merge of a term is an N-way merge in document order. The result is an index
// partitioned by term; queries (query mode, after finish) read it with no lock.
//
// The N-way merge of one term has two versions that differ in nothing else (private-merge and
// private-merge-heap): a linear scan of the heads of the k lists for every posting (O(k) per
// posting) or a binary min-heap of the heads (O(log k) per posting).
#pragma once
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "docpostings.hpp"
#include "index.hpp"
#include "partitionhash.hpp"
#include "queryengine.hpp"
#include "workers.hpp"

class PrivateMergeIndex : public Index {
public:
    // heap = false: linear-scan merge (private-merge); true: binary min-heap merge (private-merge-heap).
    explicit PrivateMergeIndex(int workers, bool heap = false) : n_(workers), heap_(heap) {}

    Worker* worker(int /*number*/) override {
        handles_.push_back(std::make_unique<Handle>(*this));
        return handles_.back().get();
    }

    const PostingList* get(const std::string& term) const override {
        const auto& map = merged_[static_cast<size_t>(partitionOf(term, n_))];
        auto it = map.find(term);
        return it == map.end() ? nullptr : &it->second;
    }

    // Merges the piles: worker i builds the merged pile i from pile i of every worker.
    void finish() override {
        merged_.assign(static_cast<size_t>(n_), Map());
        runWorkers(n_, [this](int i) { mergePile(static_cast<size_t>(i)); });
        for (auto& h : handles_) {
            docs_ += h->docs_;
            postings_ += h->postings_;
            h->piles_.clear();   // the private indexes are no longer needed
        }
    }

    int docs() const override { return docs_; }

    int terms() const override {
        size_t n = 0;
        for (const Map& m : merged_) n += m.size();
        return static_cast<int>(n);
    }

    int64_t postings() const override { return postings_; }

    void each(const std::function<void(const std::string&, const PostingList&)>& visit) const override {
        for (const Map& m : merged_)
            for (const auto& [term, list] : m) visit(term, list);
    }

private:
    using Map = std::unordered_map<std::string, PostingList>;

    // One worker's private index plus its private scratch state.
    class Handle : public Worker {
    public:
        explicit Handle(PrivateMergeIndex& x) : x_(x), piles_(static_cast<size_t>(x.n_)) {}

        void add(int docId, const std::string& text) override {
            doc_.read(text);
            for (size_t k = 0; k < doc_.size(); k++) {
                const std::string& term = *doc_.terms[k];
                Map& pile = piles_[static_cast<size_t>(partitionOf(term, x_.n_))];
                // try_emplace copies the term only when it is new
                pile.try_emplace(term).first->second.append(docId, doc_.freqs[k]);   // IDs of one worker only grow: sorted
            }
            docs_++;
            postings_ += static_cast<int64_t>(doc_.size());
        }

        int query(const Queries& q, int i) override { return engine_.run(x_, q, i); }

        void flush() override { /* nothing is buffered: the private index is the buffer */ }

    private:
        friend class PrivateMergeIndex;
        PrivateMergeIndex& x_;
        DocPostings doc_;
        QueryEngine engine_;
        std::vector<Map> piles_;   // piles_[i]: terms of pile i
        int docs_ = 0;
        int64_t postings_ = 0;
    };

    // Builds merged pile i: for every term, the N-way merge of its lists in pile i of each worker.
    // The first list of a term moves into the result with its key (no copy); the other lists of
    // the same term wait in `more` and are merged with it at the end.
    void mergePile(size_t i) {
        Map& result = merged_[i];
        std::unordered_map<std::string_view, std::vector<PostingList>> more;   // keys point into result
        for (auto& h : handles_) {
            Map& pile = h->piles_[i];
            while (!pile.empty()) {
                auto node = pile.extract(pile.begin());
                auto inserted = result.insert(std::move(node));
                if (!inserted.inserted) more[inserted.position->first].push_back(std::move(inserted.node.mapped()));
            }
        }
        for (auto& [term, lists] : more) {
            PostingList& first = result.find(std::string(term))->second;
            std::vector<const PostingList*> all{&first};
            for (const PostingList& l : lists) all.push_back(&l);
            first = heap_ ? mergeListsHeap(all) : mergeLists(all);
        }
    }

    // N-way merge of sorted lists with disjoint document IDs: repeatedly take the smallest head.
    static PostingList mergeLists(const std::vector<const PostingList*>& lists) {
        size_t total = 0;
        for (const PostingList* l : lists) total += l->size();
        PostingList out;
        out.docs.reserve(total);
        out.freqs.reserve(total);
        std::vector<size_t> at(lists.size(), 0);
        for (size_t n = 0; n < total; n++) {
            size_t best = lists.size();
            for (size_t j = 0; j < lists.size(); j++)
                if (at[j] < lists[j]->size() && (best == lists.size() || lists[j]->docs[at[j]] < lists[best]->docs[at[best]])) best = j;
            out.docs.push_back(lists[best]->docs[at[best]]);
            out.freqs.push_back(lists[best]->freqs[at[best]]);
            at[best]++;
        }
        return out;
    }

    // The same merge with a binary min-heap: it holds one entry per list that is not exhausted, the
    // head document ID (key) and the list it comes from (src), with the smallest key at the root.
    // Each posting takes the root, then replaces it by the next head of the same list or, if the
    // list is exhausted, by the last entry, and sifts it down: O(log k).
    static PostingList mergeListsHeap(const std::vector<const PostingList*>& lists) {
        size_t total = 0;
        for (const PostingList* l : lists) total += l->size();
        PostingList out;
        out.docs.reserve(total);
        out.freqs.reserve(total);
        std::vector<size_t> at(lists.size(), 0);
        std::vector<int> key(lists.size());
        std::vector<size_t> src(lists.size());
        size_t size = 0;
        for (size_t j = 0; j < lists.size(); j++) {
            if (lists[j]->size() > 0) {
                key[size] = lists[j]->docs[0];
                src[size] = j;
                size++;
            }
        }
        for (size_t i = size / 2; i-- > 0;) siftDown(key, src, size, i);   // build the heap
        for (size_t n = 0; n < total; n++) {
            size_t j = src[0];
            const PostingList* l = lists[j];
            out.docs.push_back(key[0]);
            out.freqs.push_back(l->freqs[at[j]]);
            at[j]++;
            if (at[j] < l->size()) {
                key[0] = l->docs[at[j]];   // the next head of the same list
            } else {
                size--;                    // list exhausted: the last entry takes the root
                key[0] = key[size];
                src[0] = src[size];
            }
            siftDown(key, src, size, 0);
        }
        return out;
    }

    // Moves the entry at position i down until both its children have larger keys.
    static void siftDown(std::vector<int>& key, std::vector<size_t>& src, size_t size, size_t i) {
        int k0 = key[i];
        size_t s0 = src[i];
        while (true) {
            size_t c = 2 * i + 1;                              // left child
            if (c >= size) break;
            if (c + 1 < size && key[c + 1] < key[c]) c++;      // the smaller child
            if (key[c] >= k0) break;
            key[i] = key[c];
            src[i] = src[c];
            i = c;
        }
        key[i] = k0;
        src[i] = s0;
    }

    int n_;                                    // workers = piles
    bool heap_;                                // merge with a heap instead of a linear scan
    std::vector<std::unique_ptr<Handle>> handles_;
    std::vector<Map> merged_;                  // merged_[i]: the terms of pile i
    int docs_ = 0;
    int64_t postings_ = 0;
};
