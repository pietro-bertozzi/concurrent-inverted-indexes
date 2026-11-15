// The partition-lock index (docs/experiment-plan.md, How each variant works): the index is P
// sub-maps, a term lives in partition partitionOf(term, P), and each partition has its own
// lock. A worker groups its document's postings by partition and takes each partition's lock
// once per `batch` postings (partitions in order 0 .. P-1). A query holds the locks of the
// partitions of its terms, the lower partition number first.
#pragma once
#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "docpostings.hpp"
#include "index.hpp"
#include "partitionhash.hpp"
#include "queryengine.hpp"

class PartitionLockIndex : public Index {
public:
    PartitionLockIndex(int partitions, int batch)
        : parts_(static_cast<size_t>(partitions)), batch_(static_cast<size_t>(batch)) {}

    Worker* worker() override {
        handles_.push_back(std::make_unique<Handle>(*this));
        return handles_.back().get();
    }

    const PostingList* get(const std::string& term) const override {
        const auto& map = parts_[partitionOf(term, partitions())].map;
        auto it = map.find(term);
        return it == map.end() ? nullptr : &it->second;
    }

    void finish() override { /* lists are kept sorted on every insert */ }
    int docs() const override { return docs_.load(); }

    int terms() const override {
        size_t n = 0;
        for (const Partition& p : parts_) n += p.map.size();
        return static_cast<int>(n);
    }

    int64_t postings() const override {
        int64_t n = 0;
        for (const Partition& p : parts_) n += p.postings;
        return n;
    }

    void each(const std::function<void(const std::string&, const PostingList&)>& visit) const override {
        for (const Partition& p : parts_)
            for (const auto& [term, list] : p.map) visit(term, list);
    }

private:
    // One partition: its sub-map, its lock and its posting count (guarded by the lock).
    struct Partition {
        std::unordered_map<std::string, PostingList> map;
        std::mutex mutex;               // the standard, non-fair mutex
        int64_t postings = 0;
    };

    int partitions() const { return static_cast<int>(parts_.size()); }

    // One worker's private state: tokenizing buffer, grouping arrays and query scratch space.
    class Handle : public Worker {
    public:
        explicit Handle(PartitionLockIndex& x) : x_(x), first_(x.parts_.size() + 1), next_(x.parts_.size()) {}

        void add(int docId, const std::string& text) override {
            doc_.read(text);                                  // no lock: private work
            group();
            for (size_t p = 0; p < x_.parts_.size(); p++) {
                Partition& part = x_.parts_[p];
                for (size_t from = first_[p]; from < first_[p + 1]; from += x_.batch_) {
                    size_t to = std::min(first_[p + 1], from + x_.batch_);
                    std::lock_guard<std::mutex> guard(part.mutex);
                    for (size_t s = from; s < to; s++) {
                        size_t k = order_[s];
                        part.map.try_emplace(*doc_.terms[k]).first->second.insert(docId, doc_.freqs[k]);
                    }
                    part.postings += static_cast<int64_t>(to - from);
                }
            }
            x_.docs_.fetch_add(1);
        }

        int query(const Queries& q, int i) override {
            int a = partitionOf(q.first[i], x_.partitions());
            int b = q.kind[i] == Queries::SIMPLE ? a : partitionOf(q.second[i], x_.partitions());
            int low = std::min(a, b), high = std::max(a, b);  // fixed lock order: lower number first
            std::unique_lock<std::mutex> lockLow(x_.parts_[low].mutex);
            std::unique_lock<std::mutex> lockHigh(x_.parts_[high].mutex, std::defer_lock);
            if (high != low) lockHigh.lock();
            return engine_.run(x_, q, i);
        }

    private:
        // Counting sort of the document's postings by partition: fills first_ and order_.
        void group() {
            size_t n = doc_.size(), parts = x_.parts_.size();
            partOf_.resize(n);
            order_.resize(n);
            std::fill(first_.begin(), first_.end(), 0);
            for (size_t k = 0; k < n; k++) {
                partOf_[k] = static_cast<size_t>(partitionOf(*doc_.terms[k], static_cast<int>(parts)));
                first_[partOf_[k] + 1]++;
            }
            for (size_t p = 0; p < parts; p++) first_[p + 1] += first_[p];   // first_[p] = start of partition p
            std::copy(first_.begin(), first_.begin() + static_cast<std::ptrdiff_t>(parts), next_.begin());
            for (size_t k = 0; k < n; k++) order_[next_[partOf_[k]]++] = k;
        }

        PartitionLockIndex& x_;
        DocPostings doc_;
        QueryEngine engine_;
        std::vector<size_t> first_, next_;   // first_[p] .. first_[p+1]: slots of partition p in order_
        std::vector<size_t> partOf_;         // partition of each posting of the document
        std::vector<size_t> order_;          // posting numbers, grouped by partition
    };

    std::vector<Partition> parts_;
    size_t batch_;
    std::atomic<int> docs_{0};
    std::vector<std::unique_ptr<Handle>> handles_;
};
