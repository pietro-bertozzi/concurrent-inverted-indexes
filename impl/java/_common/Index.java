import java.util.Map;

/**
 * What the shared code needs from an index. A variant implements it with its own data
 * structure and concurrency strategy; the harness (Runner) never sees the difference.
 *
 * Every worker thread gets its own {@link Worker} from {@link #worker()}; all the
 * operations that run concurrently go through it. The other methods are called by one thread
 * only, outside the concurrent phases (or, for {@link #get}, by a worker that already holds
 * the locks its variant needs).
 */
interface Index {
    /** The per-worker handle: private buffers live here, so workers never share them. */
    interface Worker {
        /** Indexes document {@code docId} (one line of the corpus: tokens separated by single spaces). */
        void add(int docId, String doc);

        /** Runs query number {@code i} of {@code q} and returns the number of matching documents. */
        int query(Queries q, int i);

        /**
         * Inserts every posting this worker still holds in a private buffer. Called by the worker's
         * own thread at the end of every phase (the indexing loop, the timed query or mixed phase).
         */
        void flush();
    }

    /** Makes the handle of worker {@code number} (0-based); called before timing starts. */
    Worker worker(int number);

    /**
     * Returns the posting list of {@code term}, sorted by document ID, or null if the term is
     * absent. No synchronisation of its own: a concurrent variant calls it with the right locks held.
     */
    PostingList get(String term);

    /** Makes the index final (sorting, merging, publishing); timed as finalize_ns. */
    void finish();

    /** Number of documents added. */
    int docs();

    /** Number of distinct terms. */
    int terms();

    /** Total number of postings over all terms. */
    long postings();

    /** All (term, posting list) pairs, in any order; used by the dump. */
    Iterable<Map.Entry<String, PostingList>> entries();
}
