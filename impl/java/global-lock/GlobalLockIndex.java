import java.util.HashMap;
import java.util.Map;
import java.util.concurrent.locks.ReentrantLock;
import java.util.concurrent.atomic.LongAdder;

/**
 * The global-lock index (docs/experiment-plan.md, How each variant works): one HashMap and one
 * lock for the whole index. A worker tokenizes its document without the lock, then takes the
 * lock and inserts up to {@code batch} postings per acquisition. A query holds the same lock
 * while it reads and merges its lists.
 */
final class GlobalLockIndex implements Index {
    private final HashMap<String, PostingList> map = new HashMap<>();
    private final ReentrantLock lock = new ReentrantLock();   // the default, non-fair lock
    private final int batch;
    private long postings = 0;                                 // guarded by lock
    private final LongAdder docs = new LongAdder();

    GlobalLockIndex(int batch) { this.batch = batch; }

    @Override
    public Worker worker() { return new Handle(); }

    /** One worker's private state: its tokenizing buffer and its query scratch space. */
    private final class Handle implements Worker {
        private final DocPostings doc = new DocPostings();
        private final QueryEngine engine = new QueryEngine();

        @Override
        public void add(int docId, String text) {
            doc.read(text);                                    // no lock: private work
            for (int from = 0; from < doc.size; from += batch) {
                int to = (int) Math.min((long) from + batch, doc.size);
                lock.lock();
                try {
                    for (int k = from; k < to; k++) insert(doc.terms[k], docId, doc.freqs[k]);
                    postings += to - from;
                } finally {
                    lock.unlock();
                }
            }
            docs.increment();
        }

        @Override
        public int query(Queries q, int i) {
            lock.lock();
            try {
                return engine.run(GlobalLockIndex.this, q, i);
            } finally {
                lock.unlock();
            }
        }
    }

    /** Adds one posting to the list of {@code term}; the caller holds the lock. */
    private void insert(String term, int docId, int freq) {
        PostingList list = map.get(term);
        if (list == null) {
            list = new PostingList();
            map.put(term, list);
        }
        list.insert(docId, freq);
    }

    @Override
    public PostingList get(String term) { return map.get(term); }

    @Override
    public void finish() { /* lists are kept sorted on every insert */ }

    @Override
    public int docs() { return docs.intValue(); }

    @Override
    public int terms() { return map.size(); }

    @Override
    public long postings() { return postings; }

    @Override
    public Iterable<Map.Entry<String, PostingList>> entries() { return map.entrySet(); }
}
