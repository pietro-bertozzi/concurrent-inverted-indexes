#include "opplan.h"

#include <stdlib.h>
#include <string.h>

#include "util.h"

int opplan_total_inserts(int ops, int share_percent) { return (int)((int64_t)ops * share_percent / 100); }

OpPlan opplan_make(int ops, int share_percent, int workers, int worker, uint64_t seed, int query_count) {
    int total_ins = opplan_total_inserts(ops, share_percent);
    int ops_here = ops / workers + (worker < ops % workers ? 1 : 0);
    int ins_here = total_ins / workers + (worker < total_ins % workers ? 1 : 0);
    SplitMix64 rng;
    splitmix64_init(&rng, seed + (uint64_t)worker);
    OpPlan plan;
    plan.query_offset = splitmix64_below(&rng, query_count);
    plan.count = ops_here;
    plan.types = xmalloc((size_t)ops_here);
    memset(plan.types, OP_QUERY, (size_t)ops_here);
    for (int i = 0; i < ins_here; i++) plan.types[i] = OP_INSERT;
    for (int j = ops_here - 1; j >= 1; j--) {
        int k = splitmix64_below(&rng, j + 1);
        uint8_t tmp = plan.types[j];
        plan.types[j] = plan.types[k];
        plan.types[k] = tmp;
    }
    plan.inserts = ins_here;
    plan.queries = ops_here - ins_here;
    return plan;
}

void opplan_free(OpPlan *plan) {
    free(plan->types);
    plan->types = NULL;
}
