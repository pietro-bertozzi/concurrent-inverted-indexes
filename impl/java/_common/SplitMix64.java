/**
 * The pseudo-random generator shared by every implementation in every language
 * (docs/design.md, section Pseudo-random generator). All values are unsigned 64-bit; Java's
 * long is signed, so shifts use ">>>" and the modulo uses Long.remainderUnsigned.
 */
final class SplitMix64 {
    private long state;

    SplitMix64(long seed) { this.state = seed; }

    /** Returns the next 64-bit value of the sequence. */
    long next() {
        state += 0x9E3779B97F4A7C15L;
        long z = state;
        z = (z ^ (z >>> 30)) * 0xBF58476D1CE4E5B9L;
        z = (z ^ (z >>> 27)) * 0x94D049BB133111EBL;
        return z ^ (z >>> 31);
    }

    /** Returns a random index below m (m > 0): next() mod m, unsigned. */
    int below(int m) { return (int) Long.remainderUnsigned(next(), m); }
}
