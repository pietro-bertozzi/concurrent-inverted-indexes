/**
 * The operations of one worker in one repetition (docs/design.md, section Exact operation
 * counts): exact numbers of inserts and queries, in a random but reproducible order.
 */
final class OpPlan {
    static final byte QUERY = 0, INSERT = 1;

    final byte[] types;      // operation types in execution order
    final int inserts;       // number of INSERT entries
    final int queries;       // number of QUERY entries
    final int queryOffset;   // where this worker starts in the query file

    private OpPlan(byte[] types, int inserts, int queryOffset) {
        this.types = types;
        this.inserts = inserts;
        this.queries = types.length - inserts;
        this.queryOffset = queryOffset;
    }

    /** Total inserts of a run: floor(ops * share / 100). */
    static int totalInserts(int ops, int sharePercent) { return (int) ((long) ops * sharePercent / 100); }

    /**
     * Builds the plan of worker {@code worker} of {@code workers}: its generator starts at
     * seed + worker; the first draw is the offset in the query file, the following ones shuffle
     * the operations with Fisher-Yates.
     */
    static OpPlan make(int ops, int sharePercent, int workers, int worker, long seed, int queryCount) {
        int totalIns = totalInserts(ops, sharePercent);
        int opsHere = ops / workers + (worker < ops % workers ? 1 : 0);
        int insHere = totalIns / workers + (worker < totalIns % workers ? 1 : 0);
        SplitMix64 rng = new SplitMix64(seed + worker);
        int offset = rng.below(queryCount);
        byte[] types = new byte[opsHere];
        for (int i = 0; i < insHere; i++) types[i] = INSERT;      // the rest stays QUERY (0)
        for (int j = opsHere - 1; j >= 1; j--) {
            int k = rng.below(j + 1);
            byte t = types[j]; types[j] = types[k]; types[k] = t;
        }
        return new OpPlan(types, insHere, offset);
    }
}
