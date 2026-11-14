import java.io.BufferedWriter;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.Arrays;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.function.Supplier;

/**
 * The benchmark harness shared by every Java variant: parses the command line, runs the
 * repetitions (warm-ups first) and prints one TSV row per repetition on stdout, in the
 * measurement columns of docs/design.md, section Results. Logs go to stderr.
 *
 * Every repetition starts from scratch: the input is read again, the index is new, the
 * generators restart from --seed, so all repetitions do identical work.
 *
 * Only one worker is supported for now (the sequential variant); the operation plan is
 * already per worker, so concurrent variants add their threads here later.
 */
final class Runner {
    private Runner() {}

    /**
     * Entry point of a variant: {@code maxThreads} is the highest worker count it supports,
     * {@code factory} makes an empty index, {@code uses} lists the numeric parameters
     * (partitions, batch, queue) the variant accepts. Exits with status 1 on error.
     */
    static void run(String[] argv, int maxThreads, Supplier<Index> factory, String... uses) {
        try {
            execute(new Args(argv), maxThreads, factory, uses);
        } catch (IllegalArgumentException | IOException e) {
            System.err.println("error: " + e.getMessage());
            System.exit(1);
        }
    }

    /** Runs all repetitions of {@code a}. */
    private static void execute(Args a, int maxThreads, Supplier<Index> factory, String[] uses) throws IOException {
        if (a.threads > maxThreads) {
            throw new IllegalArgumentException("this variant supports at most " + maxThreads + " worker(s), got --threads " + a.threads);
        }
        for (String name : new String[] {"partitions", "batch", "queue"}) {
            if (a.has(name) && !Arrays.asList(uses).contains(name)) {
                throw new IllegalArgumentException("option --" + name + " is not used by this variant");
            }
        }
        boolean build = a.mode.equals("build");
        Queries queries = build ? null : Queries.load(a.queries);
        int share = Args.insertShare(a.mode);
        System.err.println("java " + System.getProperty("java.version") + ", mode " + a.mode + ", "
                + a.threads + " worker(s), " + a.warmup + " warm-up + " + a.repeat + " repetitions");
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
                                   int share, Supplier<Index> factory, BufferedWriter latencies) throws IOException {
        boolean build = a.mode.equals("build"), mixed = a.mode.startsWith("mixed-");
        Proc.Snapshot before = Proc.snapshot();

        long t = System.nanoTime();
        List<String> docs = load(a.input);
        long loadNs = System.nanoTime() - t;
        if (docs.isEmpty()) throw new IOException(a.input + " has no documents");

        // The plan (the operation order) is made before timing starts.
        OpPlan plan = null;
        if (!build) {
            int inserts = OpPlan.totalInserts(a.ops, share);
            if (a.preload + inserts > docs.size()) {
                throw new IllegalArgumentException("mixed run needs " + (a.preload + inserts) + " documents (preload "
                        + a.preload + " + " + inserts + " inserts) but " + a.input + " has " + docs.size());
            }
            plan = OpPlan.make(a.ops, share, a.threads, 0, a.seed, queries.size);   // worker 0 (the only one)
        }

        Index index = factory.get();
        long preloadNs = 0, buildNs = 0, runNs = 0, finalizeNs = 0, resultTotal = 0;
        long[] latency = null;

        if (build || a.mode.equals("query")) {
            t = System.nanoTime();
            for (int d = 0; d < docs.size(); d++) index.add(d, docs.get(d));
            buildNs = System.nanoTime() - t;
            if (!build) {                       // queries need the finished index
                t = System.nanoTime();
                index.finish();
                finalizeNs = System.nanoTime() - t;
            }
        } else {
            t = System.nanoTime();
            for (int d = 0; d < a.preload; d++) index.add(d, docs.get(d));
            preloadNs = System.nanoTime() - t;
        }
        if (build) {
            t = System.nanoTime();
            index.finish();
            finalizeNs = System.nanoTime() - t;
        }

        if (!build) {
            latency = new long[plan.queries];
            QueryEngine engine = new QueryEngine();
            AtomicInteger nextDoc = new AtomicInteger(a.preload);   // the shared insert counter
            int cursor = plan.queryOffset, li = 0;
            t = System.nanoTime();
            for (byte type : plan.types) {
                if (type == OpPlan.QUERY) {
                    long start = System.nanoTime();
                    resultTotal += engine.run(index, queries, cursor);
                    latency[li++] = System.nanoTime() - start;
                    if (++cursor == queries.size) cursor = 0;
                } else {
                    int d = nextDoc.getAndIncrement();
                    index.add(d, docs.get(d));
                }
            }
            runNs = System.nanoTime() - t;
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
                String.valueOf(build ? 0 : plan.queries), String.valueOf(build ? 0 : plan.inserts),
                String.valueOf(index.docs()), String.valueOf(index.terms()), String.valueOf(index.postings()),
                String.valueOf(Proc.peakRssBytes()), String.valueOf(used.userNs()), String.valueOf(used.sysNs()),
                String.valueOf(used.ctxVol()), String.valueOf(used.ctxInvol()));

        // After the timers have stopped: the latencies, the row, the dump.
        if (latency != null) writeLatencies(latencies, rep, warmup, latency);
        System.out.println(row);
        System.out.flush();
        if (a.dump != null && last) Dump.write(index, a.dump);
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
