/** Java global-lock variant: one lock for the whole index; --batch postings per acquisition. */
public final class Main {
    public static void main(String[] args) {
        Runner.run(args, Integer.MAX_VALUE, a -> new GlobalLockIndex(a.batch), "batch");
    }
}
