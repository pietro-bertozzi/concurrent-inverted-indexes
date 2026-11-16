/** Java private-merge-heap variant: private-merge with a binary min-heap in the N-way merge. */
public final class Main {
    public static void main(String[] args) {
        Runner.run(args, Integer.MAX_VALUE, a -> new PrivateMergeIndex(a.threads, true));
    }
}
