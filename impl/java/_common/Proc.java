import java.io.IOException;
import java.nio.file.DirectoryStream;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.List;

/**
 * Process measurements from /proc (Linux): CPU time, context switches, peak memory.
 * CPU time has the resolution of a clock tick (10 ms; CLK_TCK is 100 on Linux, Java cannot ask
 * for it). Context switches are summed over the threads alive at the time of the reading.
 */
final class Proc {
    private static final long NS_PER_TICK = 10_000_000L;

    /** A reading of the counters that grow during the run. */
    record Snapshot(long userNs, long sysNs, long ctxVol, long ctxInvol) {
        /** The counters of this reading minus those of an earlier one. */
        Snapshot minus(Snapshot earlier) {
            return new Snapshot(userNs - earlier.userNs, sysNs - earlier.sysNs,
                    ctxVol - earlier.ctxVol, ctxInvol - earlier.ctxInvol);
        }
    }

    /** Reads the user and system CPU time of the whole process and its threads' context switches. */
    static Snapshot snapshot() throws IOException {
        // /proc/self/stat: the command name is in parentheses and may contain spaces, so
        // split after the last ')'; the next field is the state (field 3), utime is field 14.
        String stat = Files.readString(Path.of("/proc/self/stat"));
        String[] f = stat.substring(stat.lastIndexOf(')') + 2).split(" ");
        long user = Long.parseLong(f[11]) * NS_PER_TICK;
        long sys = Long.parseLong(f[12]) * NS_PER_TICK;
        long vol = 0, invol = 0;
        try (DirectoryStream<Path> tasks = Files.newDirectoryStream(Path.of("/proc/self/task"))) {
            for (Path task : tasks) {
                List<String> lines;
                try {
                    lines = Files.readAllLines(task.resolve("status"));
                } catch (IOException e) {
                    continue;   // the thread ended while we were looking
                }
                for (String line : lines) {
                    if (line.startsWith("voluntary_ctxt_switches:")) vol += value(line);
                    else if (line.startsWith("nonvoluntary_ctxt_switches:")) invol += value(line);
                }
            }
        }
        return new Snapshot(user, sys, vol, invol);
    }

    /** The peak resident memory of the process so far, in bytes (VmHWM). */
    static long peakRssBytes() throws IOException {
        for (String line : Files.readAllLines(Path.of("/proc/self/status"))) {
            if (line.startsWith("VmHWM:")) return value(line) * 1024;   // reported in kB
        }
        throw new IOException("VmHWM not found in /proc/self/status");
    }

    /** The number after the colon of a /proc/.../status line. */
    private static long value(String line) {
        return Long.parseLong(line.substring(line.indexOf(':') + 1).replaceAll("[^0-9]", ""));
    }
}
