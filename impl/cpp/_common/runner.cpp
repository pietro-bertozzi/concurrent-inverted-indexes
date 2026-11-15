#include "runner.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <stdexcept>

#include "args.hpp"
#include "dump.hpp"
#include "opplan.hpp"
#include "proc.hpp"
#include "queries.hpp"
#include "queryengine.hpp"
#include "workers.hpp"

namespace {

using Clock = std::chrono::steady_clock;

// Nanoseconds from t to now.
int64_t since(Clock::time_point t) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t).count();
}

// Mean cost in ns of one Clock::now(): 1,000,000 back-to-back readings (docs/design.md, Query latencies).
int64_t clockCostNs() {
    const int n = 1000000;
    uint64_t sink = 0;   // unsigned, so the sum of the readings may wrap
    auto start = Clock::now();
    for (int i = 0; i < n; i++) sink += static_cast<uint64_t>(Clock::now().time_since_epoch().count());
    int64_t total = since(start);
    if (sink == 42) std::cerr << "";   // the readings are consumed, so they cannot be removed
    return total / n;
}

// Reads the input file: one string per line (document ID = position).
std::vector<std::string> load(const std::string& file) {
    std::ifstream in(file);
    if (!in) throw std::runtime_error("cannot open " + file);
    std::vector<std::string> docs;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        docs.push_back(line);
    }
    return docs;
}

// Appends the latencies of one repetition to the latency file: rep, warmup, latency_ns.
void writeLatencies(std::ofstream& out, int rep, int warmup, const std::vector<int64_t>& latency) {
    std::string prefix = std::to_string(rep) + "\t" + std::to_string(warmup) + "\t", text;
    for (int64_t ns : latency) text += prefix + std::to_string(ns) + "\n";
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

// One worker's share of the timed phase: follows its plan, timing every query with two clock
// readings around it. An insert takes the next document from the shared counter (whichever
// worker inserts next takes the next document). Returns the sum of the query result sizes.
int64_t runPlan(Worker& me, const OpPlan& plan, const Queries& queries, const std::vector<std::string>& docs,
                std::atomic<int>& nextDoc, std::vector<int64_t>& latency) {
    int64_t total = 0;
    int cursor = plan.queryOffset;
    size_t li = 0;
    for (uint8_t type : plan.types) {
        if (type == OpPlan::QUERY) {
            auto start = Clock::now();
            total += me.query(queries, cursor);
            latency[li++] = since(start);
            if (++cursor == queries.size()) cursor = 0;
        } else {
            int d = nextDoc.fetch_add(1);
            me.add(d, docs[d]);
        }
    }
    me.flush();   // the end of the phase: nothing may stay in a private buffer
    return total;
}

// One repetition: load, index (build / preload), run, finish; prints its row.
void repetition(const Args& a, int rep, bool warmup, bool last, const Queries* queries,
                const IndexFactory& factory, std::ofstream* latencies) {
    bool build = a.mode == "build", mixed = a.mode.rfind("mixed-", 0) == 0;
    int share = insertShare(a.mode);
    ProcSnapshot before = procSnapshot();

    auto t = Clock::now();
    std::vector<std::string> docs = load(a.input);
    int64_t loadNs = since(t);
    if (docs.empty()) throw std::runtime_error(a.input + " has no documents");
    int docCount = static_cast<int>(docs.size());

    // The plans (the operation order of every worker) are made before timing starts.
    int n = a.threads;
    std::vector<OpPlan> plans;
    int64_t opsQuery = 0, opsInsert = 0;
    if (!build) {
        int inserts = OpPlan::totalInserts(a.ops, share);
        if (static_cast<int64_t>(a.preload) + inserts > docCount) {
            throw std::runtime_error("mixed run needs " + std::to_string(static_cast<int64_t>(a.preload) + inserts) +
                                     " documents (preload " + std::to_string(a.preload) + " + " + std::to_string(inserts) +
                                     " inserts) but " + a.input + " has " + std::to_string(docCount));
        }
        for (int w = 0; w < n; w++) {
            plans.push_back(OpPlan::make(a.ops, share, n, w, a.seed, queries->size()));
            opsQuery += plans[w].queries;
            opsInsert += plans[w].inserts;
        }
    }

    std::unique_ptr<Index> index = factory(a);
    std::vector<Worker*> workers;   // one handle per worker, made before timing
    for (int w = 0; w < n; w++) workers.push_back(index->worker(w));
    int64_t preloadNs = 0, buildNs = 0, runNs = 0, finalizeNs = 0, resultTotal = 0;
    std::vector<std::vector<int64_t>> latency;

    if (build || a.mode == "query") {
        t = Clock::now();
        indexDocs(workers, docs, 0, docCount);
        buildNs = since(t);
        if (!build) {                       // queries need the finished index
            t = Clock::now();
            index->finish();
            finalizeNs = since(t);
        }
    } else {
        t = Clock::now();
        indexDocs(workers, docs, 0, a.preload);
        preloadNs = since(t);
    }
    if (build) {
        t = Clock::now();
        index->finish();
        finalizeNs = since(t);
    }

    if (!build) {
        latency.resize(n);
        for (int w = 0; w < n; w++) latency[w].resize(plans[w].queries);
        std::vector<int64_t> results(n, 0);
        std::atomic<int> nextDoc(a.preload);   // the shared insert counter
        t = Clock::now();
        runWorkers(n, [&](int w) { results[w] = runPlan(*workers[w], plans[w], *queries, docs, nextDoc, latency[w]); });
        runNs = since(t);
        for (int64_t r : results) resultTotal += r;
        if (mixed) {
            t = Clock::now();
            index->finish();
            finalizeNs = since(t);
        }
        std::cerr << "rep " << rep << ": sum of query result sizes = " << resultTotal << "\n";
    }

    ProcSnapshot used = procSnapshot().minus(before);
    int64_t peak = peakRssBytes();
    int w = warmup ? 1 : 0;
    std::string row;
    for (int64_t v : {int64_t(rep), int64_t(w), loadNs, preloadNs, buildNs, runNs, finalizeNs,
                      opsQuery, opsInsert,
                      int64_t(index->docs()), int64_t(index->terms()), index->postings(), peak,
                      used.userNs, used.sysNs, used.ctxVol, used.ctxInvol}) {
        if (!row.empty()) row += '\t';
        row += std::to_string(v);
    }

    // After the timers have stopped: the latencies, the row, the dump.
    for (const auto& l : latency) writeLatencies(*latencies, rep, w, l);
    std::cout << row << "\n" << std::flush;
    if (!a.dump.empty() && last) writeDump(*index, a.dump);
}

// Runs all repetitions of a.
void execute(const Args& a, int maxThreads, const IndexFactory& factory,
             const std::vector<std::string>& uses) {
    if (a.threads > maxThreads) {
        throw std::runtime_error("this variant supports at most " + std::to_string(maxThreads) +
                                 " worker(s), got --threads " + std::to_string(a.threads));
    }
    for (const char* name : {"partitions", "batch", "queue"}) {
        bool used = std::find(uses.begin(), uses.end(), name) != uses.end();
        if (a.has(name) && !used) throw std::runtime_error(std::string("option --") + name + " is not used by this variant");
        if (!a.has(name) && used) throw std::runtime_error(std::string("missing required option --") + name + " for this variant");
    }
    bool build = a.mode == "build";
    Queries queries;
    if (!build) queries = loadQueries(a.queries);
    std::cerr << "c++ g++ " << __VERSION__ << ", mode " << a.mode << ", " << a.threads << " worker(s), "
              << a.warmup << " warm-up + " << a.repeat << " repetitions\n";
    std::cerr << "clock reading cost: " << clockCostNs() << " ns (mean of 1000000 back-to-back readings)\n";
    std::ofstream latencies;
    if (!build) {
        latencies.open(a.latencies, std::ios::binary);
        if (!latencies) throw std::runtime_error("cannot write " + a.latencies);
    }
    int total = a.warmup + a.repeat;
    for (int rep = 0; rep < total; rep++) {
        repetition(a, rep, rep < a.warmup, rep == total - 1, build ? nullptr : &queries, factory,
                   build ? nullptr : &latencies);
    }
    if (!build) {
        latencies.flush();
        if (!latencies) throw std::runtime_error("error while writing " + a.latencies);
    }
}

}  // namespace

int runVariant(int argc, char** argv, int maxThreads, const IndexFactory& factory,
               const std::vector<std::string>& uses) {
    try {
        execute(parseArgs(std::vector<std::string>(argv + 1, argv + argc)), maxThreads, factory, uses);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
