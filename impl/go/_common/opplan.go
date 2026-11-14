package main

// Operation types of an OpPlan.
const (
	OpQuery  byte = 0
	OpInsert byte = 1
)

// OpPlan holds the operations of one worker in one repetition (docs/design.md, section Exact
// operation counts): exact numbers of inserts and queries, in a random but reproducible order.
type OpPlan struct {
	Types       []byte // operation types in execution order
	Inserts     int    // number of OpInsert entries
	Queries     int    // number of OpQuery entries
	QueryOffset int    // where this worker starts in the query file
}

// TotalInserts is the number of inserts of a whole run: floor(ops * share / 100).
func TotalInserts(ops int, sharePercent int) int { return int(int64(ops) * int64(sharePercent) / 100) }

// MakeOpPlan builds the plan of worker `worker` of `workers`: its generator starts at
// seed + worker; the first draw is the offset in the query file, the following ones shuffle
// the operations with Fisher-Yates.
func MakeOpPlan(ops, sharePercent, workers, worker int, seed uint64, queryCount int) *OpPlan {
	totalIns := TotalInserts(ops, sharePercent)
	opsHere := ops / workers
	if worker < ops%workers {
		opsHere++
	}
	insHere := totalIns / workers
	if worker < totalIns%workers {
		insHere++
	}
	rng := NewSplitMix64(seed + uint64(worker))
	offset := rng.Below(queryCount)
	types := make([]byte, opsHere) // zero is OpQuery
	for i := 0; i < insHere; i++ {
		types[i] = OpInsert
	}
	for j := opsHere - 1; j >= 1; j-- {
		k := rng.Below(j + 1)
		types[j], types[k] = types[k], types[j]
	}
	return &OpPlan{Types: types, Inserts: insHere, Queries: opsHere - insHere, QueryOffset: offset}
}
