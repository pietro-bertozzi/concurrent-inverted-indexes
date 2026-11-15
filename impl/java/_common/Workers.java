import java.util.List;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.IntConsumer;

/**
 * The worker loop shared by every variant (docs/experiment-plan.md, Rules every variant
 * follows): N threads run the same body, and documents are handed out dynamically from a
 * shared counter. A worker that finishes a document takes the next one, so faster cores
 * simply take more documents.
 */
final class Workers {
    private Workers() {}

    /** Runs {@code body(w)} for w = 0 .. n-1, each on its own thread, and waits for all of them. */
    static void run(int n, IntConsumer body) {
        Thread[] threads = new Thread[n];
        AtomicReference<Throwable> failure = new AtomicReference<>();
        for (int w = 0; w < n; w++) {
            final int worker = w;
            threads[w] = new Thread(() -> {
                try {
                    body.accept(worker);
                } catch (Throwable t) {
                    failure.compareAndSet(null, t);
                }
            }, "worker-" + w);
            threads[w].start();
        }
        for (Thread t : threads) {
            try {
                t.join();
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                throw new IllegalStateException("interrupted while waiting for the workers", e);
            }
        }
        if (failure.get() != null) throw new IllegalStateException("a worker failed: " + failure.get(), failure.get());
    }

    /**
     * Indexes the documents {@code from} (inclusive) to {@code to} (exclusive): each worker
     * repeatedly takes the next document ID from a shared atomic counter (d = counter++) and
     * adds it. The counter is touched once per document, not once per word, so it costs almost nothing.
     */
    static void index(Index.Worker[] workers, List<String> docs, int from, int to) {
        AtomicInteger next = new AtomicInteger(from);
        run(workers.length, w -> {
            Index.Worker me = workers[w];
            int d;
            while ((d = next.getAndIncrement()) < to) me.add(d, docs.get(d));
            me.flush();   // the end of the phase: nothing may stay in a private buffer
        });
    }
}
