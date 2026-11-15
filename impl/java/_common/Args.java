import java.util.HashMap;
import java.util.Map;
import java.util.Set;

/**
 * The command line of docs/design.md, section CLI contract: long options only, each with
 * one value; an unknown option, a missing value, a missing required option or an option that
 * does not belong to the mode is an error.
 */
final class Args {
    private static final Set<String> KNOWN = Set.of("mode", "input", "threads", "repeat", "warmup",
            "queries", "ops", "preload", "seed", "partitions", "batch", "queue", "latencies", "dump");

    final String mode;          // build, query, mixed-95-5, mixed-50-50, mixed-20-80
    final String input;
    final int threads, repeat, warmup;
    final String queries;       // query and mixed modes
    final int ops;              // query and mixed modes
    final int preload;          // mixed modes
    final long seed;            // query and mixed modes
    final String latencies;     // query and mixed modes
    final String dump;          // optional, null if absent
    final int partitions, batch, queue;   // numeric parameters; 0 if absent (a variant checks the ones it uses)
    private final Map<String, String> values;

    /** Parses and validates {@code argv}; throws IllegalArgumentException with the reason. */
    Args(String[] argv) {
        values = new HashMap<>();
        for (int i = 0; i < argv.length; i += 2) {
            if (!argv[i].startsWith("--")) throw new IllegalArgumentException("expected an option, got '" + argv[i] + "'");
            String name = argv[i].substring(2);
            if (!KNOWN.contains(name)) throw new IllegalArgumentException("unknown option " + argv[i]);
            if (i + 1 >= argv.length) throw new IllegalArgumentException("option " + argv[i] + " has no value");
            if (values.put(name, argv[i + 1]) != null) throw new IllegalArgumentException("option " + argv[i] + " given twice");
        }
        mode = required("mode");
        boolean query = mode.equals("query"), mixed = mode.startsWith("mixed-");
        if (!mode.equals("build") && !query && !(mixed && insertShare(mode) >= 0)) {
            throw new IllegalArgumentException("unknown mode '" + mode + "'");
        }
        input = required("input");
        threads = integer("threads", 1);
        repeat = integer("repeat", 1);
        warmup = integer("warmup", 0);
        // Options that only some modes use: required there, an error elsewhere.
        for (String name : new String[] {"queries", "ops", "seed", "latencies"}) {
            if (query || mixed) required(name); else forbidden(name);
        }
        if (mixed) required("preload"); else forbidden("preload");
        queries = values.get("queries");
        latencies = values.get("latencies");
        ops = (query || mixed) ? integer("ops", 0) : 0;
        preload = mixed ? integer("preload", 0) : 0;
        seed = (query || mixed) ? Long.parseUnsignedLong(values.get("seed")) : 0;
        dump = values.get("dump");
        partitions = values.containsKey("partitions") ? integer("partitions", 1) : 0;
        batch = values.containsKey("batch") ? integer("batch", 1) : 0;
        queue = values.containsKey("queue") ? integer("queue", 1) : 0;
    }

    /** The insert share in percent of {@code mode}: 0 for query, 100 for build, 5/50/80 for mixed; -1 if not a mode. */
    static int insertShare(String mode) {
        if (mode.equals("build")) return 100;
        if (mode.equals("query")) return 0;
        if (mode.matches("mixed-\\d+-\\d+")) {
            int share = Integer.parseInt(mode.substring(mode.lastIndexOf('-') + 1));
            return share <= 100 ? share : -1;
        }
        return -1;
    }

    /** True if the command line carries the numeric parameter {@code name} (partitions, batch, queue). */
    boolean has(String name) { return values.containsKey(name); }

    private String required(String name) {
        String v = values.get(name);
        if (v == null) throw new IllegalArgumentException("missing required option --" + name + " for mode " + (values.get("mode")));
        return v;
    }

    private void forbidden(String name) {
        if (values.containsKey(name)) throw new IllegalArgumentException("option --" + name + " does not apply to mode " + values.get("mode"));
    }

    private int integer(String name, int min) {
        String v = required(name);
        try {
            int n = Integer.parseInt(v);
            if (n < min) throw new NumberFormatException();
            return n;
        } catch (NumberFormatException e) {
            throw new IllegalArgumentException("option --" + name + " must be an integer >= " + min + ", got '" + v + "'");
        }
    }
}
