// The operations of one worker in one repetition (docs/design.md, section Exact operation
// counts): exact numbers of inserts and queries, in a random but reproducible order.
#pragma once
#include <cstdint>
#include <utility>
#include <vector>

#include "splitmix64.hpp"

struct OpPlan {
    static constexpr uint8_t QUERY = 0, INSERT = 1;

    std::vector<uint8_t> types;   // operation types in execution order
    int inserts = 0;              // number of INSERT entries
    int queries = 0;              // number of QUERY entries
    int queryOffset = 0;          // where this worker starts in the query file

    // Total inserts of a run: floor(ops * share / 100).
    static int totalInserts(int ops, int sharePercent) {
        return static_cast<int>(static_cast<int64_t>(ops) * sharePercent / 100);
    }

    // Builds the plan of worker `worker` of `workers`: its generator starts at seed + worker;
    // the first draw is the offset in the query file, the following ones shuffle the
    // operations with Fisher-Yates.
    static OpPlan make(int ops, int sharePercent, int workers, int worker, uint64_t seed, int queryCount) {
        int totalIns = totalInserts(ops, sharePercent);
        int opsHere = ops / workers + (worker < ops % workers ? 1 : 0);
        int insHere = totalIns / workers + (worker < totalIns % workers ? 1 : 0);
        SplitMix64 rng(seed + static_cast<uint64_t>(worker));
        OpPlan plan;
        plan.queryOffset = rng.below(queryCount);
        plan.types.assign(opsHere, QUERY);
        for (int i = 0; i < insHere; i++) plan.types[i] = INSERT;
        for (int j = opsHere - 1; j >= 1; j--) {
            int k = rng.below(j + 1);
            std::swap(plan.types[j], plan.types[k]);
        }
        plan.inserts = insHere;
        plan.queries = opsHere - insHere;
        return plan;
    }
};
