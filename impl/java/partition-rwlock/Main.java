/** Java partition-rwlock variant: P sub-maps by term, one read/write lock each; --batch postings per acquisition. */
public final class Main {
    public static void main(String[] args) {
        Runner.run(args, Integer.MAX_VALUE, a -> new PartitionRwLockIndex(a.partitions, a.batch), "partitions", "batch");
    }
}
