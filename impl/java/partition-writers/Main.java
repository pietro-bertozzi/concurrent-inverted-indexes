/** Java partition-writers variant: P sub-maps by term, one owner thread each; postings travel in bounded queues. */
public final class Main {
    public static void main(String[] args) {
        Runner.run(args, Integer.MAX_VALUE, a -> new PartitionWritersIndex(a.partitions, a.batch, a.queue, a.threads),
                "partitions", "batch", "queue");
    }
}
