/** Java private-merge variant: a private index per worker, merged in parallel at the end (finalize). */
public final class Main {
    public static void main(String[] args) {
        Runner.run(args, Integer.MAX_VALUE, a -> new PrivateMergeIndex(a.threads));
    }
}
