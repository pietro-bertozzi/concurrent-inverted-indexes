// The partition-writers index (docs/experiment-plan.md, How each variant works): the index is P
// sub-maps by term, and every partition has exactly one owner, the only thread that ever writes
// it. The owners are the workers themselves: partition p belongs to worker p mod N. A worker
// groups its document's postings by partition into one private buffer per partition; a buffer
// that holds `batch` postings becomes a message. A message for a partition the worker owns is
// applied at once; any other goes into the owner's bounded queue (`queue` messages). A worker
// empties its own queue after every document and whenever it has to wait.
#pragma once
#include <algorithm>
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "docpostings.hpp"
#include "index.hpp"
#include "partitionhash.hpp"
#include "postingbuffer.hpp"
#include "queryengine.hpp"

class PartitionWritersIndex : public Index {
public:
    PartitionWritersIndex(int partitions, int batch, int queue, int workers)
        : parts_(static_cast<size_t>(partitions)), batch_(static_cast<size_t>(batch)),
          workers_(workers), queues_(static_cast<size_t>(workers)) {
        for (Queue& q : queues_) q.capacity = static_cast<size_t>(queue);
    }

    Worker* worker(int number) override {
        handles_.push_back(std::make_unique<Handle>(*this, number));
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
    // One partition: its sub-map and its posting count. Only its owner writes them.
    struct Partition {
        std::unordered_map<std::string, PostingList> map;
        int64_t postings = 0;
    };

    // A message: a full buffer of postings, all for one partition.
    struct Message {
        size_t partition = 0;
        PostingBuffer postings;
    };

    // A worker's mailbox: a bounded queue protected by a mutex. Any worker may push, only the
    // owner pops; both operations are non-blocking (they say whether they succeeded).
    struct Queue {
        std::mutex mutex;
        std::deque<Message> messages;
        size_t capacity = 0;

        // Moves `m` into the queue and returns true, or returns false (m untouched) if it is full.
        bool tryPush(Message& m) {
            std::lock_guard<std::mutex> guard(mutex);
            if (messages.size() >= capacity) return false;
            messages.push_back(std::move(m));
            return true;
        }

        // Takes the oldest message into `m` and returns true, or returns false if the queue is empty.
        bool tryPop(Message& m) {
            std::lock_guard<std::mutex> guard(mutex);
            if (messages.empty()) return false;
            m = std::move(messages.front());
            messages.pop_front();
            return true;
        }
    };

    int partitions() const { return static_cast<int>(parts_.size()); }

    // Inserts the postings of `buffer` into partition p; only the owner of p calls it. The terms
    // are moved into the map, so a new term costs no second copy.
    void apply(size_t p, PostingBuffer& buffer) {
        Partition& part = parts_[p];
        for (size_t k = 0; k < buffer.size(); k++)
            part.map.try_emplace(std::move(buffer.terms[k])).first->second.insert(buffer.docs[k], buffer.freqs[k]);
        part.postings += static_cast<int64_t>(buffer.size());
    }

    // One worker's private state: tokenizing buffer, grouping arrays, one posting buffer per
    // partition and query scratch space.
    class Handle : public Worker {
    public:
        Handle(PartitionWritersIndex& x, int number)
            : x_(x), number_(static_cast<size_t>(number)), start_(static_cast<size_t>(number) % x.parts_.size()),
              buffers_(x.parts_.size()), first_(x.parts_.size() + 1), next_(x.parts_.size()) {
            for (PostingBuffer& b : buffers_) reserve(b);
        }

        void add(int docId, const std::string& text) override {
            doc_.read(text);                                  // private work
            group();
            for (size_t i = 0; i < x_.parts_.size(); i++) {   // partitions from start_, wrapping around
                size_t p = (start_ + i) % x_.parts_.size();
                for (size_t s = first_[p]; s < first_[p + 1]; s++) {
                    size_t k = order_[s];
                    buffers_[p].add(*doc_.terms[k], docId, doc_.freqs[k]);
                    if (buffers_[p].size() == x_.batch_) send(p);   // full: one message of `batch` postings
                }
            }
            x_.docs_.fetch_add(1);
            drain();                                          // regularly empty our own mailbox
        }

        // Ends the phase. First send everything still buffered. Then wait until every worker has
        // done the same, emptying our own queue meanwhile: once all have sent, nothing new can
        // arrive, and one last drain applies what is left. Every worker calls flush once per phase.
        void flush() override {
            for (size_t i = 0; i < buffers_.size(); i++) {
                size_t p = (start_ + i) % buffers_.size();
                if (buffers_[p].size() > 0) send(p);
            }
            phases_++;
            x_.finished_.fetch_add(1);
            while (x_.finished_.load() < phases_ * x_.workers_) {
                if (!drain()) std::this_thread::yield();      // nothing to apply: let others run
            }
            drain();                                          // every message was queued before its sender's increment
        }

        int query(const Queries& q, int i) override {
            return engine_.run(x_, q, i);                     // nobody writes in query mode: no lock
        }

    private:
        // Gives a buffer room for min(batch, 1024) postings.
        void reserve(PostingBuffer& b) const {
            size_t c = std::min<size_t>(x_.batch_, 1024);
            b.terms.reserve(c);
            b.docs.reserve(c);
            b.freqs.reserve(c);
        }

        // Hands the buffer of partition p to its owner: applied directly if that is us, else queued.
        void send(size_t p) {
            size_t owner = p % static_cast<size_t>(x_.workers_);
            if (owner == number_) {
                x_.apply(p, buffers_[p]);
                buffers_[p].clear();
                return;
            }
            Message m{p, std::move(buffers_[p])};             // the full buffer travels; no copy of the postings
            buffers_[p] = PostingBuffer();
            reserve(buffers_[p]);
            while (!x_.queues_[owner].tryPush(m)) {           // queue full: backpressure
                if (!drain()) std::this_thread::yield();      // deadlock rule: empty our own queue before waiting
            }
        }

        // Applies every message in our own queue; returns true if there was any.
        bool drain() {
            bool any = false;
            Message m;
            while (x_.queues_[number_].tryPop(m)) {
                x_.apply(m.partition, m.postings);
                any = true;
            }
            return any;
        }

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

        PartitionWritersIndex& x_;
        size_t number_;                        // this worker
        size_t start_;                         // the partition visited first (worker number mod P)
        long phases_ = 0;                      // flush calls made so far
        DocPostings doc_;
        QueryEngine engine_;
        std::vector<PostingBuffer> buffers_;   // postings waiting for each partition
        std::vector<size_t> first_, next_;     // first_[p] .. first_[p+1]: slots of partition p in order_
        std::vector<size_t> partOf_;           // partition of each posting of the document
        std::vector<size_t> order_;            // posting numbers, grouped by partition
    };

    std::vector<Partition> parts_;
    size_t batch_;
    long workers_;
    std::vector<Queue> queues_;                // queues_[w]: the mailbox of worker w
    std::atomic<long> finished_{0};            // flush calls that have sent everything, over all phases
    std::atomic<int> docs_{0};
    std::vector<std::unique_ptr<Handle>> handles_;
};
