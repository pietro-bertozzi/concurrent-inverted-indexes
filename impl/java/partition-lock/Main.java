/** Java partition-lock variant: P sub-maps by term, one lock each; --batch postings per acquisition. */
public final class Main {
    public static void main(String[] args) {
        Runner.run(args, Integer.MAX_VALUE, a -> new PartitionLockIndex(a.partitions, a.batch), "partitions", "batch");
    }
}
