import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * The private-merge index (docs/experiment-plan.md, How each variant works). While building,
 * every worker writes only into its own private index, with no lock: its terms are already
 * divided into N piles by the partition hash (P = N, the number of workers). In finish() worker i
 * merges pile i of every worker, so all workers merge at once, each on different terms, with no
 * lock either. A worker takes increasing document IDs, so each private list is already sorted
 * and the merge of a term is an N-way merge in document order. The result is an index
 * partitioned by term; queries (query mode, after finish) read it with no lock.
 */
final class PrivateMergeIndex implements Index {
    private final int n;                        // workers = piles
    private final List<Handle> handles = new ArrayList<>();
    private final List<HashMap<String, PostingList>> merged = new ArrayList<>();   // merged.get(i): the terms of pile i
    private int docs = 0;                       // set by finish()
    private long postings = 0;

    PrivateMergeIndex(int workers) { this.n = workers; }

    @Override
    public Worker worker(int number) {
        Handle h = new Handle();
        handles.add(h);
        return h;
    }

    /** One worker's private index plus its private scratch state. */
    private final class Handle implements Worker {
        private final DocPostings doc = new DocPostings();
        private final QueryEngine engine = new QueryEngine();
        private final List<HashMap<String, PostingList>> piles = new ArrayList<>();   // piles.get(i): terms of pile i
        private int docs = 0;
        private long postings = 0;

        Handle() {
            for (int i = 0; i < n; i++) piles.add(new HashMap<>());
        }

        @Override
        public void add(int docId, String text) {
            doc.read(text);
            for (int k = 0; k < doc.size; k++) {
                String term = doc.terms[k];
                HashMap<String, PostingList> pile = piles.get(PartitionHash.of(term, n));
                PostingList list = pile.get(term);
                if (list == null) {
                    list = new PostingList();
                    pile.put(term, list);
                }
                list.append(docId, doc.freqs[k]);   // IDs of one worker only grow: the list stays sorted
            }
            docs++;
            postings += doc.size;
        }

        @Override
        public int query(Queries q, int i) { return engine.run(PrivateMergeIndex.this, q, i); }

        @Override
        public void flush() { /* nothing is buffered: the private index is the buffer */ }
    }

    /** Merges the piles: worker i builds the merged pile i from pile i of every worker. */
    @Override
    public void finish() {
        for (int i = 0; i < n; i++) merged.add(null);
        Workers.run(n, this::mergePile);
        for (Handle h : handles) {
            docs += h.docs;
            postings += h.postings;
            h.piles.clear();   // the private indexes are no longer needed
        }
    }

    /** Builds merged pile {@code i}: for every term, the N-way merge of its lists in pile i of each worker. */
    private void mergePile(int i) {
        HashMap<String, List<PostingList>> parts = new HashMap<>();
        for (Handle h : handles) {
            for (Map.Entry<String, PostingList> e : h.piles.get(i).entrySet()) {
                parts.computeIfAbsent(e.getKey(), t -> new ArrayList<>(2)).add(e.getValue());
            }
        }
        HashMap<String, PostingList> result = new HashMap<>(parts.size() * 2);
        for (Map.Entry<String, List<PostingList>> e : parts.entrySet()) {
            List<PostingList> lists = e.getValue();
            result.put(e.getKey(), lists.size() == 1 ? lists.get(0) : merge(lists));   // one list: reuse it
        }
        merged.set(i, result);   // each worker writes its own slot; join() publishes it
    }

    /** N-way merge of sorted lists with disjoint document IDs: repeatedly take the smallest head. */
    private static PostingList merge(List<PostingList> lists) {
        int k = lists.size(), total = 0;
        for (PostingList l : lists) total += l.size;
        PostingList out = new PostingList();
        out.docs = new int[total];
        out.freqs = new int[total];
        int[] at = new int[k];
        for (int n = 0; n < total; n++) {
            int best = -1;
            for (int j = 0; j < k; j++) {
                PostingList l = lists.get(j);
                if (at[j] < l.size && (best < 0 || l.docs[at[j]] < lists.get(best).docs[at[best]])) best = j;
            }
            PostingList l = lists.get(best);
            out.docs[n] = l.docs[at[best]];
            out.freqs[n] = l.freqs[at[best]];
            at[best]++;
        }
        out.size = total;
        return out;
    }

    @Override
    public PostingList get(String term) { return merged.get(PartitionHash.of(term, n)).get(term); }

    @Override
    public int docs() { return docs; }

    @Override
    public int terms() {
        int t = 0;
        for (HashMap<String, PostingList> m : merged) t += m == null ? 0 : m.size();
        return t;
    }

    @Override
    public long postings() { return postings; }

    @Override
    public Iterable<Map.Entry<String, PostingList>> entries() {
        List<Map.Entry<String, PostingList>> all = new ArrayList<>(terms());
        for (HashMap<String, PostingList> m : merged) all.addAll(m.entrySet());
        return all;
    }
}
