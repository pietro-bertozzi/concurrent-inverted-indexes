import java.lang.foreign.Arena;
import java.lang.foreign.FunctionDescriptor;
import java.lang.foreign.Linker;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.lang.invoke.MethodHandle;
import java.io.IOException;

/**
 * Process measurements from getrusage(RUSAGE_SELF), called through the Foreign Function &
 * Memory API: the same call every language uses (docs/design.md, section Results), so the
 * numbers are comparable. RUSAGE_SELF covers all threads of the process, also those that
 * have already ended.
 *
 * Layout of struct rusage on 64-bit Linux (all fields 8 bytes): ru_utime and ru_stime are
 * struct timeval (seconds, microseconds) at offsets 0 and 16; ru_maxrss (kilobytes) is at
 * offset 32; ru_nvcsw and ru_nivcsw (voluntary and involuntary context switches) at 128
 * and 136; the struct is 144 bytes long.
 */
final class Proc {
    private static final int RUSAGE_SELF = 0;
    private static final long SIZE = 144;
    private static final MethodHandle GETRUSAGE;

    static {
        Linker linker = Linker.nativeLinker();
        MemorySegment symbol = linker.defaultLookup().find("getrusage")
                .orElseThrow(() -> new IllegalStateException("getrusage not found in the C library"));
        GETRUSAGE = linker.downcallHandle(symbol,
                FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.JAVA_INT, ValueLayout.ADDRESS));
    }

    /** A reading of the counters that grow during the run. */
    record Snapshot(long userNs, long sysNs, long ctxVol, long ctxInvol) {
        /** The counters of this reading minus those of an earlier one. */
        Snapshot minus(Snapshot earlier) {
            return new Snapshot(userNs - earlier.userNs, sysNs - earlier.sysNs,
                    ctxVol - earlier.ctxVol, ctxInvol - earlier.ctxInvol);
        }
    }

    /** Reads user and system CPU time (ns) and the context switches of the whole process. */
    static Snapshot snapshot() throws IOException {
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment ru = call(arena);
            return new Snapshot(
                    ru.get(ValueLayout.JAVA_LONG, 0) * 1_000_000_000L + ru.get(ValueLayout.JAVA_LONG, 8) * 1_000L,
                    ru.get(ValueLayout.JAVA_LONG, 16) * 1_000_000_000L + ru.get(ValueLayout.JAVA_LONG, 24) * 1_000L,
                    ru.get(ValueLayout.JAVA_LONG, 128), ru.get(ValueLayout.JAVA_LONG, 136));
        }
    }

    /** The peak resident memory of the process so far, in bytes (ru_maxrss, reported in kB). */
    static long peakRssBytes() throws IOException {
        try (Arena arena = Arena.ofConfined()) {
            return call(arena).get(ValueLayout.JAVA_LONG, 32) * 1024;
        }
    }

    /** Calls getrusage(RUSAGE_SELF) into a fresh struct allocated in {@code arena}. */
    private static MemorySegment call(Arena arena) throws IOException {
        MemorySegment ru = arena.allocate(SIZE);
        int status;
        try {
            status = (int) GETRUSAGE.invokeExact(RUSAGE_SELF, ru);
        } catch (Throwable e) {
            throw new IOException("getrusage failed: " + e, e);
        }
        if (status != 0) throw new IOException("getrusage returned " + status);
        return ru;
    }
}
