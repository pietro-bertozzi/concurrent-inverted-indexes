import java.util.HashMap;
import java.util.Map;
import java.util.concurrent.locks.ReentrantLock;
import java.util.concurrent.atomic.LongAdder;

/**
 * The global-lock index (docs/experiment-plan.md, How each variant works): one HashMap and one
 * lock for the whole index. A worker tokenizes its document without the lock and collects the
 * postings in a private buffer; when the buffer holds {@code batch} postings (from one or
 * several documents) it takes the lock and inserts them all, and it flushes what is left at the
 * end of every phase. A query holds the same lock while it reads and merges its lists.
 */
final class GlobalLockIndex implements Index {
    private final HashMap<String, PostingList> map = new HashMap<>();
    private final ReentrantLock lock = new ReentrantLock();   // the default, non-fair lock
    private final int batch;
    private long postings = 0;                                 // guarded by lock
    private final LongAdder docs = new LongAdder();

    GlobalLockIndex(int batch) { this.batch = batch; }

    @Override
    public Worker worker(int number) { return new Handle(); }

    /** One worker's private state: its tokenizing buffer, its posting buffer and its query scratch space. */
    private final class Handle implements Worker {
        private final DocPostings doc = new DocPostings();
        private final PostingBuffer buffer = new PostingBuffer();
        private final QueryEngine engine = new QueryEngine();

        @Override
        public void add(int docId, String text) {
            doc.read(text);                                    // no lock: private work
            for (int k = 0; k < doc.size; k++) {
                buffer.add(doc.terms[k], docId, doc.freqs[k]);
                if (buffer.size == batch) flushBuffer();       // full: one lock acquisition for `batch` postings
            }
            docs.increment();
        }

        @Override
        public void flush() { if (buffer.size > 0) flushBuffer(); }

        /** Inserts the whole buffer under one acquisition of the lock. */
        private void flushBuffer() {
            lock.lock();
            try {
                for (int k = 0; k < buffer.size; k++) insert(buffer.terms[k], buffer.docs[k], buffer.freqs[k]);
                postings += buffer.size;
            } finally {
                lock.unlock();
            }
            buffer.clear();
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
