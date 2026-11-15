import java.util.HashMap;
import java.util.Map;

/**
 * The sequential index: one HashMap from term to posting list, no synchronisation. Documents
 * are added in increasing ID order, so every posting list is already sorted and finish()
 * has nothing to do.
 */
final class SequentialIndex implements Index, Index.Worker {
    private final HashMap<String, PostingList> map = new HashMap<>();
    private int docs = 0;
    private long postings = 0;
    private final QueryEngine engine = new QueryEngine();

    @Override
    public void add(int docId, String doc) {
        Tokenizer tokens = new Tokenizer(doc);
        while (tokens.next()) {
            String term = tokens.token();
            PostingList list = map.get(term);
            if (list == null) {
                list = new PostingList();
                map.put(term, list);
            }
            // A term seen earlier in this document is the last posting of its list: count it.
            if (list.size > 0 && list.docs[list.size - 1] == docId) {
                list.freqs[list.size - 1]++;
            } else {
                list.append(docId, 1);
                postings++;
            }
        }
        docs++;
    }

    /** Only one worker exists: the index is its own handle. */
    @Override
    public Worker worker() { return this; }

    @Override
    public int query(Queries q, int i) { return engine.run(this, q, i); }

    @Override
    public PostingList get(String term) { return map.get(term); }

    @Override
    public void finish() { /* sorted by construction */ }

    @Override
    public int docs() { return docs; }

    @Override
    public int terms() { return map.size(); }

    @Override
    public long postings() { return postings; }

    @Override
    public Iterable<Map.Entry<String, PostingList>> entries() { return map.entrySet(); }
}
