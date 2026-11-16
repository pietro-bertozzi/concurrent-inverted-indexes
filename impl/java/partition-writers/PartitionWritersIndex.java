import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.LongAdder;

/**
 * The partition-writers index (docs/experiment-plan.md, How each variant works): the index is P
 * sub-maps by term, and every partition has exactly one owner, the only thread that ever writes
 * it. The owners are the workers themselves: partition p belongs to worker p mod N. A worker
 * groups its document's postings by partition into one private buffer per partition; a buffer that
 * holds {@code batch} postings becomes a message. A message for a partition the worker owns is
 * applied at once; any other goes into the owner's bounded queue ({@code queue} messages). A
 * worker empties its own queue after every document and whenever it has to wait.
 */
final class PartitionWritersIndex implements Index {
    /** One partition: its sub-map and its posting count. Only the owner writes them. */
    private static final class Partition {
        final HashMap<String, PostingList> map = new HashMap<>();
        long postings = 0;
    }

    /** A message: a full buffer of postings, all for one partition. */
    private static final class Message {
        final int partition;
        final PostingBuffer postings;

        Message(int partition, PostingBuffer postings) {
            this.partition = partition;
            this.postings = postings;
        }
    }

    private final Partition[] parts;
    private final int batch;
    private final int workers;
    private final ArrayBlockingQueue<Message>[] queues;   // queues[w]: the mailbox of worker w
    private final AtomicInteger finished = new AtomicInteger();   // flush calls that have sent everything, over all phases
    private final LongAdder docs = new LongAdder();

    // Java cannot create an array of a generic type: the raw array only ever holds ArrayBlockingQueue<Message>.
    @SuppressWarnings({"unchecked", "rawtypes"})
    PartitionWritersIndex(int partitions, int batch, int queue, int workers) {
        this.parts = new Partition[partitions];
        for (int p = 0; p < partitions; p++) parts[p] = new Partition();
        this.batch = batch;
        this.workers = workers;
        this.queues = new ArrayBlockingQueue[workers];
        for (int w = 0; w < workers; w++) queues[w] = new ArrayBlockingQueue<>(queue);
    }

    @Override
    public Worker worker(int number) { return new Handle(number); }

    /** One worker's private state: tokenizing buffer, grouping arrays, one posting buffer per partition, query scratch space. */
    private final class Handle implements Worker {
        private final DocPostings doc = new DocPostings();
        private final QueryEngine engine = new QueryEngine();
        private final PostingBuffer[] buffers = new PostingBuffer[parts.length];   // postings waiting for each partition
        private final int[] first = new int[parts.length + 1];   // first[p] .. first[p+1]: slots of partition p in order[]
        private int[] partOf = new int[512];                     // partition of each posting of the document
        private int[] order = new int[512];                      // posting numbers, grouped by partition
        private final int number;                                // this worker; also the start of its partition order
        private final int start;                                 // the partition visited first (worker number mod P)
        private final int capacity = Math.min(batch, 1024);      // initial room of a buffer
        private int phases = 0;                                  // flush calls made so far

        Handle(int number) {
            this.number = number;
            this.start = number % parts.length;
            for (int p = 0; p < buffers.length; p++) buffers[p] = new PostingBuffer(capacity);
        }

        @Override
        public void add(int docId, String text) {
            doc.read(text);                                       // private work
            group();
            for (int i = 0; i < parts.length; i++) {              // partitions from `start`, wrapping around
                int p = (start + i) % parts.length;
                for (int s = first[p]; s < first[p + 1]; s++) {
                    int k = order[s];
                    buffers[p].add(doc.terms[k], docId, doc.freqs[k]);   // read the slot every time: send() replaces the buffer
                    if (buffers[p].size == batch) send(p);        // full: one message of `batch` postings
                }
            }
            docs.increment();
            drain();                                              // regularly empty our own mailbox
        }

        /**
         * Ends the phase. First send everything still buffered. Then wait until every worker has
         * done the same, emptying our own queue meanwhile: once all have sent, nothing new can
         * arrive, and one last drain applies what is left. Every worker calls flush once per phase.
         */
        @Override
        public void flush() {
            for (int i = 0; i < parts.length; i++) {
                int p = (start + i) % parts.length;
                if (buffers[p].size > 0) send(p);
            }
            phases++;
            finished.incrementAndGet();
            while (finished.get() < phases * workers) {
                if (!drain()) Thread.yield();                     // nothing to apply: let others run
            }
            drain();                                              // every message was queued before its sender's increment
        }

        /** Hands the buffer of partition {@code p} to its owner: applied directly if that is us, else queued. */
        private void send(int p) {
            int owner = p % workers;
            PostingBuffer buffer = buffers[p];
            if (owner == number) {
                apply(p, buffer);
                buffer.clear();
                return;
            }
            buffers[p] = new PostingBuffer(capacity);             // the full buffer travels; no copy
            Message message = new Message(p, buffer);
            while (!queues[owner].offer(message)) {               // queue full: backpressure
                if (!drain()) Thread.yield();                     // deadlock rule: empty our own queue before waiting
            }
        }

        /** Applies every message in our own queue; returns true if there was any. */
        private boolean drain() {
            boolean any = false;
            Message m;
            while ((m = queues[number].poll()) != null) {
                apply(m.partition, m.postings);
                any = true;
            }
            return any;
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
            return engine.run(PartitionWritersIndex.this, q, i);   // nobody writes in query mode: no lock
        }
    }

    /** Inserts the postings of {@code buffer} into partition {@code p}; only the owner of {@code p} calls it. */
    private void apply(int p, PostingBuffer buffer) {
        Partition part = parts[p];
        for (int k = 0; k < buffer.size; k++) {
            PostingList list = part.map.get(buffer.terms[k]);
            if (list == null) {
                list = new PostingList();
                part.map.put(buffer.terms[k], list);
            }
            list.insert(buffer.docs[k], buffer.freqs[k]);
        }
        part.postings += buffer.size;
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
