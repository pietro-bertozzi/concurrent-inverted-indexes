/** A posting list: a growable array of (document ID, term frequency) pairs, kept as two int arrays. */
final class PostingList {
    int[] docs = new int[2];
    int[] freqs = new int[2];
    int size = 0;

    /** Appends a posting; the caller appends in increasing document ID order. */
    void append(int docId, int freq) {
        if (size == docs.length) {
            docs = java.util.Arrays.copyOf(docs, size * 2);
            freqs = java.util.Arrays.copyOf(freqs, size * 2);
        }
        docs[size] = docId;
        freqs[size] = freq;
        size++;
    }

    /**
     * Inserts a posting in its correct place, keeping the list sorted (docs/design.md, section
     * Posting lists stay sorted). With dynamic handout documents arrive nearly in order, so the
     * place is almost always the end (a plain append) or a few slots before it. The document
     * ID must not be in the list yet.
     */
    void insert(int docId, int freq) {
        int pos = size;
        while (pos > 0 && docs[pos - 1] > docId) pos--;   // walk back from the end
        append(docId, freq);                               // grows the arrays if needed
        if (pos < size - 1) {                              // not the end: shift the tail one slot up
            System.arraycopy(docs, pos, docs, pos + 1, size - 1 - pos);
            System.arraycopy(freqs, pos, freqs, pos + 1, size - 1 - pos);
            docs[pos] = docId;
            freqs[pos] = freq;
        }
    }
}
