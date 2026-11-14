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
}
