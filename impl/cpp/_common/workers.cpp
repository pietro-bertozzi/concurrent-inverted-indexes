#include "workers.hpp"

#include <atomic>
#include <exception>
#include <mutex>
#include <thread>

void runWorkers(int n, const std::function<void(int)>& body) {
    std::vector<std::thread> threads;
    std::exception_ptr failure;
    std::mutex failureMutex;
    for (int w = 0; w < n; w++) {
        threads.emplace_back([&, w] {
            try {
                body(w);
            } catch (...) {
                std::lock_guard<std::mutex> guard(failureMutex);
                if (!failure) failure = std::current_exception();
            }
        });
    }
    for (std::thread& t : threads) t.join();
    if (failure) std::rethrow_exception(failure);
}

void indexDocs(const std::vector<Worker*>& workers, const std::vector<std::string>& docs, int from, int to) {
    std::atomic<int> next(from);
    runWorkers(static_cast<int>(workers.size()), [&](int w) {
        int d;
        while ((d = next.fetch_add(1)) < to) workers[w]->add(d, docs[d]);
        workers[w]->flush();   // the end of the phase: nothing may stay in a private buffer
    });
}
