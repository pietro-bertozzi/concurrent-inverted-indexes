import java.io.BufferedWriter;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.Arrays;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.function.Function;

/**
 * The benchmark harness shared by every Java variant: parses the command line, runs the
 * repetitions (warm-ups first) and prints one TSV row per repetition on stdout, in the
 * measurement columns of docs/design.md, section Results. Logs go to stderr.
 *
 * Every repetition starts from scratch: the input is read again, the index is new, the
 * generators restart from --seed, so all repetitions do identical work.
 *
 * Every phase runs on N worker threads (also with N = 1): documents are handed out from a
 * shared counter (Workers), and each worker follows its own operation plan (OpPlan).
 */
final class Runner {
    private Runner() {}

    /**
     * Entry point of a variant: {@code maxThreads} is the highest worker count it supports,
     * {@code factory} makes an empty index from the command line (the variant reads the numeric
     * parameters it uses), {@code uses} lists those parameters (partitions, batch, queue).
     * Exits with status 1 on error.
     */
    static void run(String[] argv, int maxThreads, Function<Args, Index> factory, String... uses) {
        try {
            execute(new Args(argv), maxThreads, factory, uses);
        } catch (IllegalArgumentException | IOException e) {
            System.err.println("error: " + e.getMessage());
            System.exit(1);
        }
    }

    /** Mean cost in ns of one System.nanoTime() call: 1,000,000 back-to-back readings (docs/design.md, Query latencies). */
    private static long clockCostNs() {
        final int n = 1_000_000;
        long sink = 0;
        long start = System.nanoTime();
        for (int i = 0; i < n; i++) sink += System.nanoTime();
        long total = System.nanoTime() - start;
        if (sink == 42) System.err.print("");   // the readings are consumed, so they cannot be removed
        return total / n;
    }

    /** Runs all repetitions of {@code a}. */
    private static void execute(Args a, int maxThreads, Function<Args, Index> factory, String[] uses) throws IOException {
        if (a.threads > maxThreads) {
            throw new IllegalArgumentException("this variant supports at most " + maxThreads + " worker(s), got --threads " + a.threads);
        }
        for (String name : new String[] {"partitions", "batch", "queue"}) {
            boolean used = Arrays.asList(uses).contains(name);
            if (a.has(name) && !used) throw new IllegalArgumentException("option --" + name + " is not used by this variant");
            if (!a.has(name) && used) throw new IllegalArgumentException("missing required option --" + name + " for this variant");
        }
        boolean build = a.mode.equals("build");
        Queries queries = build ? null : Queries.load(a.queries);
        int share = Args.insertShare(a.mode);
        System.err.println("java " + System.getProperty("java.version") + ", mode " + a.mode + ", "
                + a.threads + " worker(s), " + a.warmup + " warm-up + " + a.repeat + " repetitions");
        System.err.println("clock reading cost: " + clockCostNs() + " ns (mean of 1000000 back-to-back readings)");
        BufferedWriter latencies = build ? null
                : Files.newBufferedWriter(Path.of(a.latencies), StandardCharsets.UTF_8);
        try {
            int total = a.warmup + a.repeat;
            for (int rep = 0; rep < total; rep++) {
                repetition(a, rep, rep < a.warmup, rep == total - 1, queries, share, factory, latencies);
            }
        } finally {
            if (latencies != null) latencies.close();
        }
    }

    /** One repetition: load, index (build / preload), run, finish; prints its row. */
    private static void repetition(Args a, int rep, boolean warmup, boolean last, Queries queries,
                                   int share, Function<Args, Index> factory, BufferedWriter latencies) throws IOException {
        boolean build = a.mode.equals("build"), mixed = a.mode.startsWith("mixed-");
        Proc.Snapshot before = Proc.snapshot();

        long t = System.nanoTime();
        List<String> docs = load(a.input);
        long loadNs = System.nanoTime() - t;
        if (docs.isEmpty()) throw new IOException(a.input + " has no documents");

        // The plans (the operation order of every worker) are made before timing starts.
        int n = a.threads;
        OpPlan[] plans = null;
        int opsQuery = 0, opsInsert = 0;
        if (!build) {
            int inserts = OpPlan.totalInserts(a.ops, share);
            if (a.preload + inserts > docs.size()) {
                throw new IllegalArgumentException("mixed run needs " + (a.preload + inserts) + " documents (preload "
                        + a.preload + " + " + inserts + " inserts) but " + a.input + " has " + docs.size());
            }
            plans = new OpPlan[n];
            for (int w = 0; w < n; w++) {
                plans[w] = OpPlan.make(a.ops, share, n, w, a.seed, queries.size);
                opsQuery += plans[w].queries;
                opsInsert += plans[w].inserts;
            }
        }

        Index index = factory.apply(a);
        Index.Worker[] workers = new Index.Worker[n];   // one handle per worker, made before timing
        for (int w = 0; w < n; w++) workers[w] = index.worker();
        long preloadNs = 0, buildNs = 0, runNs = 0, finalizeNs = 0, resultTotal = 0;
        long[][] latency = null;

        if (build || a.mode.equals("query")) {
            t = System.nanoTime();
            Workers.index(workers, docs, 0, docs.size());
            buildNs = System.nanoTime() - t;
            if (!build) {                       // queries need the finished index
                t = System.nanoTime();
                index.finish();
                finalizeNs = System.nanoTime() - t;
            }
        } else {
            t = System.nanoTime();
            Workers.index(workers, docs, 0, a.preload);
            preloadNs = System.nanoTime() - t;
        }
        if (build) {
            t = System.nanoTime();
            index.finish();
            finalizeNs = System.nanoTime() - t;
        }

        if (!build) {
            final OpPlan[] myPlans = plans;
            final long[][] lat = new long[n][];
            for (int w = 0; w < n; w++) lat[w] = new long[plans[w].queries];
            latency = lat;
            long[] results = new long[n];
            AtomicInteger nextDoc = new AtomicInteger(a.preload);   // the shared insert counter
            t = System.nanoTime();
            Workers.run(n, w -> results[w] = runPlan(workers[w], myPlans[w], queries, docs, nextDoc, lat[w]));
            runNs = System.nanoTime() - t;
            for (long r : results) resultTotal += r;
            if (mixed) {
                t = System.nanoTime();
                index.finish();
                finalizeNs = System.nanoTime() - t;
            }
            System.err.println("rep " + rep + ": sum of query result sizes = " + resultTotal);
        }

        Proc.Snapshot used = Proc.snapshot().minus(before);
        String row = String.join("\t", String.valueOf(rep), warmup ? "1" : "0",
                String.valueOf(loadNs), String.valueOf(preloadNs), String.valueOf(buildNs),
                String.valueOf(runNs), String.valueOf(finalizeNs),
                String.valueOf(opsQuery), String.valueOf(opsInsert),
                String.valueOf(index.docs()), String.valueOf(index.terms()), String.valueOf(index.postings()),
                String.valueOf(Proc.peakRssBytes()), String.valueOf(used.userNs()), String.valueOf(used.sysNs()),
                String.valueOf(used.ctxVol()), String.valueOf(used.ctxInvol()));

        // After the timers have stopped: the latencies, the row, the dump.
        if (latency != null) for (long[] l : latency) writeLatencies(latencies, rep, warmup, l);
        System.out.println(row);
        System.out.flush();
        if (a.dump != null && last) Dump.write(index, a.dump);
    }

    /**
     * One worker's share of the timed phase: follows its plan, timing every query with two clock
     * readings around it. An insert takes the next document from the shared counter (whichever
     * worker inserts next takes the next document). Returns the sum of the query result sizes.
     */
    private static long runPlan(Index.Worker me, OpPlan plan, Queries queries, List<String> docs,
                                AtomicInteger nextDoc, long[] latency) {
        long total = 0;
        int cursor = plan.queryOffset, li = 0;
        for (byte type : plan.types) {
            if (type == OpPlan.QUERY) {
                long start = System.nanoTime();
                total += me.query(queries, cursor);
                latency[li++] = System.nanoTime() - start;
                if (++cursor == queries.size) cursor = 0;
            } else {
                int d = nextDoc.getAndIncrement();
                me.add(d, docs.get(d));
            }
        }
        return total;
    }

    /** Reads the input file: one string per line (document ID = position). */
    private static List<String> load(String file) throws IOException {
        List<String> docs = new ArrayList<>();
        try (var in = Files.newBufferedReader(Path.of(file), StandardCharsets.UTF_8)) {
            String line;
            while ((line = in.readLine()) != null) docs.add(line);
        }
        return docs;
    }

    /** Appends the latencies of one repetition to the latency file: rep, warmup, latency_ns. */
    private static void writeLatencies(BufferedWriter out, int rep, boolean warmup, long[] latency) throws IOException {
        String prefix = rep + "\t" + (warmup ? 1 : 0) + "\t";
        for (long ns : latency) out.write(prefix + ns + "\n");
    }
}
