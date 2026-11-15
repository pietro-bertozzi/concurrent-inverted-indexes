// The global-lock index (docs/experiment-plan.md, How each variant works): one unordered_map
// and one lock for the whole index. A worker tokenizes its document without the lock, then
// takes the lock and inserts up to `batch` postings per acquisition. A query holds the same
// lock while it reads and merges its lists.
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
#include "queryengine.hpp"

class GlobalLockIndex : public Index {
public:
    explicit GlobalLockIndex(int batch) : batch_(static_cast<size_t>(batch)) {}

    Worker* worker() override {
        handles_.push_back(std::make_unique<Handle>(*this));
        return handles_.back().get();
    }

    const PostingList* get(const std::string& term) const override {
        auto it = map_.find(term);
        return it == map_.end() ? nullptr : &it->second;
    }

    void finish() override { /* lists are kept sorted on every insert */ }
    int docs() const override { return docs_.load(); }
    int terms() const override { return static_cast<int>(map_.size()); }
    int64_t postings() const override { return postings_; }

    void each(const std::function<void(const std::string&, const PostingList&)>& visit) const override {
        for (const auto& [term, list] : map_) visit(term, list);
    }

private:
    // One worker's private state: its tokenizing buffer and its query scratch space.
    class Handle : public Worker {
    public:
        explicit Handle(GlobalLockIndex& x) : x_(x) {}

        void add(int docId, const std::string& text) override {
            doc_.read(text);                                  // no lock: private work
            for (size_t from = 0; from < doc_.size(); from += x_.batch_) {
                size_t to = std::min(doc_.size(), from + x_.batch_);
                std::lock_guard<std::mutex> guard(x_.mutex_);
                for (size_t k = from; k < to; k++) x_.insert(*doc_.terms[k], docId, doc_.freqs[k]);
                x_.postings_ += static_cast<int64_t>(to - from);
            }
            x_.docs_.fetch_add(1);
        }

        int query(const Queries& q, int i) override {
            std::lock_guard<std::mutex> guard(x_.mutex_);
            return engine_.run(x_, q, i);
        }

    private:
        GlobalLockIndex& x_;
        DocPostings doc_;
        QueryEngine engine_;
    };

    // Adds one posting to the list of term; the caller holds the lock.
    void insert(const std::string& term, int docId, int freq) {
        map_.try_emplace(term).first->second.insert(docId, freq);
    }

    std::unordered_map<std::string, PostingList> map_;
    std::mutex mutex_;                  // the standard, non-fair mutex
    size_t batch_;
    int64_t postings_ = 0;              // guarded by mutex_
    std::atomic<int> docs_{0};
    std::vector<std::unique_ptr<Handle>> handles_;
};
