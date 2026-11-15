import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.atomic.LongAdder;
import java.util.concurrent.locks.ReentrantLock;

/**
 * The partition-lock index (docs/experiment-plan.md, How each variant works): the index is P
 * sub-maps, a term lives in partition PartitionHash.of(term, P), and each partition has its own
 * lock. A worker groups its document's postings by partition into one private buffer per
 * partition; when a buffer holds {@code batch} postings (from one or several documents) it takes
 * that partition's lock and inserts them all, and it flushes what is left at the end of every
 * phase, visiting the partitions from (worker number mod P), wrapping around, so that workers do not all queue at partition 0. A query holds the locks of the partitions of its terms, the lower partition number first.
 */
final class PartitionLockIndex implements Index {
    /** One partition: its sub-map, its lock and its posting count (guarded by the lock). */
    private static final class Partition {
        final HashMap<String, PostingList> map = new HashMap<>();
        final ReentrantLock lock = new ReentrantLock();   // the default, non-fair lock
        long postings = 0;
    }

    private final Partition[] parts;
    private final int batch;
    private final LongAdder docs = new LongAdder();

    PartitionLockIndex(int partitions, int batch) {
        this.parts = new Partition[partitions];
        for (int p = 0; p < partitions; p++) parts[p] = new Partition();
        this.batch = batch;
    }

    @Override
    public Worker worker(int number) { return new Handle(number % parts.length); }

    /** One worker's private state: tokenizing buffer, grouping arrays, one posting buffer per partition, query scratch space. */
    private final class Handle implements Worker {
        private final DocPostings doc = new DocPostings();
        private final QueryEngine engine = new QueryEngine();
        private final PostingBuffer[] buffers = new PostingBuffer[parts.length];   // postings waiting for each partition
        private final int[] first = new int[parts.length + 1];   // first[p] .. first[p+1]: slots of partition p in order[]
        private int[] partOf = new int[512];                     // partition of each posting of the document
        private int[] order = new int[512];                      // posting numbers, grouped by partition

        private final int start;   // the partition this worker visits first (worker number mod P)

        Handle(int start) {
            this.start = start;
            for (int p = 0; p < buffers.length; p++) buffers[p] = new PostingBuffer();
        }

        @Override
        public void add(int docId, String text) {
            doc.read(text);                                       // no lock: private work
            group();
            for (int i = 0; i < parts.length; i++) {              // partitions from `start`, wrapping around
                int p = (start + i) % parts.length;
                PostingBuffer buffer = buffers[p];
                for (int s = first[p]; s < first[p + 1]; s++) {
                    int k = order[s];
                    buffer.add(doc.terms[k], docId, doc.freqs[k]);
                    if (buffer.size == batch) flushPartition(p);  // full: one acquisition for `batch` postings
                }
            }
            docs.increment();
        }

        @Override
        public void flush() {
            for (int i = 0; i < parts.length; i++) {
                int p = (start + i) % parts.length;
                if (buffers[p].size > 0) flushPartition(p);
            }
        }

        /** Inserts the buffer of partition {@code p} under one acquisition of that partition's lock. */
        private void flushPartition(int p) {
            Partition part = parts[p];
            PostingBuffer buffer = buffers[p];
            part.lock.lock();
            try {
                for (int k = 0; k < buffer.size; k++) insert(part, buffer.terms[k], buffer.docs[k], buffer.freqs[k]);
                part.postings += buffer.size;
            } finally {
                part.lock.unlock();
            }
            buffer.clear();
        }

        /** Counting sort of the document's postings by partition: fills first[] and order[]. */
        private void group() {
            int n = doc.size;
            if (partOf.length < n) {
                partOf = new int[n * 2];
                order = new int[n * 2];
            }
            java.util.Arrays.fill(first, 0);
            for (int k = 0; k < n; k++) {
                partOf[k] = PartitionHash.of(doc.terms[k], parts.length);
                first[partOf[k] + 1]++;
            }
            for (int p = 0; p < parts.length; p++) first[p + 1] += first[p];   // first[p] = start of partition p
            int[] next = java.util.Arrays.copyOf(first, parts.length);
            for (int k = 0; k < n; k++) order[next[partOf[k]]++] = k;
        }

        @Override
        public int query(Queries q, int i) {
            int a = PartitionHash.of(q.first[i], parts.length);
            int b = q.kind[i] == Queries.SIMPLE ? a : PartitionHash.of(q.second[i], parts.length);
            int low = Math.min(a, b), high = Math.max(a, b);      // fixed lock order: lower number first
            parts[low].lock.lock();
            if (high != low) parts[high].lock.lock();
            try {
                return engine.run(PartitionLockIndex.this, q, i);
            } finally {
                if (high != low) parts[high].lock.unlock();
                parts[low].lock.unlock();
            }
        }
    }

    /** Adds one posting to the list of {@code term} in {@code part}; the caller holds its lock. */
    private static void insert(Partition part, String term, int docId, int freq) {
        PostingList list = part.map.get(term);
        if (list == null) {
            list = new PostingList();
            part.map.put(term, list);
        }
        list.insert(docId, freq);
    }

    @Override
    public PostingList get(String term) { return parts[PartitionHash.of(term, parts.length)].map.get(term); }

    @Override
    public void finish() { /* lists are kept sorted on every insert */ }

    @Override
    public int docs() { return docs.intValue(); }

    @Override
    public int terms() {
        int n = 0;
        for (Partition p : parts) n += p.map.size();
        return n;
    }

    @Override
    public long postings() {
        long n = 0;
        for (Partition p : parts) n += p.postings;
        return n;
    }

    @Override
    public Iterable<Map.Entry<String, PostingList>> entries() {
        List<Map.Entry<String, PostingList>> all = new ArrayList<>(terms());
        for (Partition p : parts) all.addAll(p.map.entrySet());
        return all;
    }
}
