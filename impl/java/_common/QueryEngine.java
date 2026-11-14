/**
 * Evaluates queries (docs/design.md, section Queries). The result of a query is the set of
 * document IDs; the engine returns its size, which the caller adds up so that the work cannot
 * be optimised away. AND and OR merge the two sorted lists with two pointers into a scratch
 * array that the engine keeps between calls (one engine per worker).
 */
final class QueryEngine {
    private static final PostingList EMPTY = new PostingList();
    private int[] scratch = new int[1024];

    /** Runs query number {@code i} of {@code q} on {@code index} and returns the number of matching documents. */
    int run(Index index, Queries q, int i) {
        PostingList a = list(index, q.first[i]);
        if (q.kind[i] == Queries.SIMPLE) return a.size;
        PostingList b = list(index, q.second[i]);
        return q.kind[i] == Queries.AND ? intersect(a, b) : union(a, b);
    }

    private static PostingList list(Index index, String term) {
        PostingList l = index.get(term);
        return l == null ? EMPTY : l;
    }

    /** Intersection of two sorted lists: advance the pointer with the smaller ID, keep equal IDs. */
    private int intersect(PostingList a, PostingList b) {
        reserve(Math.min(a.size, b.size));
        int i = 0, j = 0, n = 0;
        while (i < a.size && j < b.size) {
            int x = a.docs[i], y = b.docs[j];
            if (x == y) { scratch[n++] = x; i++; j++; }
            else if (x < y) i++;
            else j++;
        }
        return n;
    }

    /** Union of two sorted lists: always take the smaller ID, an ID present in both is taken once. */
    private int union(PostingList a, PostingList b) {
        reserve(a.size + b.size);
        int i = 0, j = 0, n = 0;
        while (i < a.size && j < b.size) {
            int x = a.docs[i], y = b.docs[j];
            if (x == y) { scratch[n++] = x; i++; j++; }
            else if (x < y) { scratch[n++] = x; i++; }
            else { scratch[n++] = y; j++; }
        }
        while (i < a.size) scratch[n++] = a.docs[i++];
        while (j < b.size) scratch[n++] = b.docs[j++];
        return n;
    }

    /** Makes sure the scratch array can hold {@code n} IDs. */
    private void reserve(int n) {
        if (scratch.length < n) scratch = new int[Math.max(n, scratch.length * 2)];
    }
}
