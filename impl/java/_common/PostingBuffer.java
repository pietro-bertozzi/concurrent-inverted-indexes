import java.util.Arrays;

/**
 * A worker's private buffer of postings waiting to be inserted into the shared index
 * (docs/experiment-plan.md, Rules every variant follows, Batching). Postings from several
 * documents collect here; the variant flushes the buffer, under its lock, when it holds
 * {@code batch} postings and at the end of every phase.
 */
final class PostingBuffer {
    String[] terms = new String[64];
    int[] docs = new int[64];
    int[] freqs = new int[64];
    int size = 0;

    /** Appends one posting (term, document, frequency). */
    void add(String term, int docId, int freq) {
        if (size == terms.length) {
            terms = Arrays.copyOf(terms, size * 2);
            docs = Arrays.copyOf(docs, size * 2);
            freqs = Arrays.copyOf(freqs, size * 2);
        }
        terms[size] = term;
        docs[size] = docId;
        freqs[size] = freq;
        size++;
    }

    /** Empties the buffer (the arrays are kept for reuse). */
    void clear() { size = 0; }
}
