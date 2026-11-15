import java.util.Arrays;

/**
 * A worker's private buffer of postings waiting to be inserted into the shared index
 * (docs/experiment-plan.md, Rules every variant follows, Batching). Postings from several
 * documents collect here; the variant flushes the buffer, under its lock, when it holds
 * {@code batch} postings and at the end of every phase.
 */
final class PostingBuffer {
    String[] terms;
    int[] docs;
    int[] freqs;
    int size = 0;

    PostingBuffer() { this(64); }

    /** A buffer with room for {@code capacity} postings before it has to grow. */
    PostingBuffer(int capacity) {
        terms = new String[capacity];
        docs = new int[capacity];
        freqs = new int[capacity];
    }

    /** Appends one posting (term, document, frequency). */
    void add(String term, int docId, int freq) {
        if (size == terms.length) {
            terms = Arrays.copyOf(terms, size * 2 + 1);
            docs = Arrays.copyOf(docs, size * 2 + 1);
            freqs = Arrays.copyOf(freqs, size * 2 + 1);
        }
        terms[size] = term;
        docs[size] = docId;
        freqs[size] = freq;
        size++;
    }

    /** Empties the buffer (the arrays are kept for reuse). */
    void clear() { size = 0; }
}
