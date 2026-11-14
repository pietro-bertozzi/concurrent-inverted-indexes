import java.util.Map;

/**
 * What the shared code needs from an index. A variant implements it with its own data
 * structure and concurrency strategy; the harness (Runner) never sees the difference.
 */
interface Index {
    /** Indexes document {@code docId} (one line of the corpus: tokens separated by single spaces). */
    void add(int docId, String doc);

    /** Returns the posting list of {@code term}, sorted by document ID, or null if the term is absent. */
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
