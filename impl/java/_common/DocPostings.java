import java.util.HashMap;
import java.util.Map;

/**
 * The postings of one document: its distinct terms with their frequencies. This is the
 * tokenizing job of the concurrent variants (docs/experiment-plan.md, section Each structural
 * dimension): private to a worker, no shared data. One object per worker, reused for every
 * document.
 */
final class DocPostings {
    private final HashMap<String, int[]> counts = new HashMap<>();
    String[] terms = new String[512];   // distinct terms of the last document read, [0, size)
    int[] freqs = new int[512];         // their frequencies
    int size = 0;

    /** Replaces the contents with the postings of {@code doc}. */
    void read(String doc) {
        counts.clear();
        Tokenizer tokens = new Tokenizer(doc);
        while (tokens.next()) {
            int[] count = counts.get(tokens.token());
            if (count == null) counts.put(tokens.token(), new int[] {1});
            else count[0]++;
        }
        size = 0;
        if (counts.size() > terms.length) {
            terms = new String[counts.size() * 2];
            freqs = new int[counts.size() * 2];
        }
        for (Map.Entry<String, int[]> e : counts.entrySet()) {
            terms[size] = e.getKey();
            freqs[size] = e.getValue()[0];
            size++;
        }
    }
}
