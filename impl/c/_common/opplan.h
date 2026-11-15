// The operations of one worker in one repetition (docs/design.md, section Exact operation
// counts): exact numbers of inserts and queries, in a random but reproducible order.
#pragma once
#include <stdint.h>

#include "splitmix64.h"

enum { OP_QUERY = 0, OP_INSERT = 1 };

typedef struct {
    uint8_t *types;     // operation types in execution order, count entries
    int count;
    int inserts;        // number of OP_INSERT entries
    int queries;        // number of OP_QUERY entries
    int query_offset;   // where this worker starts in the query file
} OpPlan;

// Total inserts of a run: floor(ops * share / 100).
int opplan_total_inserts(int ops, int share_percent);

// Builds the plan of worker `worker` of `workers`: its generator starts at seed + worker; the
// first draw is the offset in the query file, the following ones shuffle the operations with
// Fisher-Yates. Free it with opplan_free.
OpPlan opplan_make(int ops, int share_percent, int workers, int worker, uint64_t seed, int query_count);

// Frees the operation array of a plan.
void opplan_free(OpPlan *plan);
