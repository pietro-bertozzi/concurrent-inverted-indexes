/**
 * The partition of a term (docs/design.md, section Partition hash): FNV-1a 64-bit over the
 * term's UTF-8 bytes, then (hash >> 32) mod P. The same in every language, so each partition
 * holds the same terms everywhere.
 */
final class PartitionHash {
    private PartitionHash() {}

    private static final long OFFSET_BASIS = 0xcbf29ce484222325L, PRIME = 0x100000001b3L;

    /** One FNV-1a step: xor the byte in, multiply by the prime (64-bit wrapping). */
    private static long step(long h, int b) { return (h ^ b) * PRIME; }

    /** FNV-1a 64-bit of the UTF-8 encoding of {@code term}, computed without building the byte array. */
    static long hash(String term) {
        long h = OFFSET_BASIS;
        for (int i = 0; i < term.length(); ) {
            int cp = term.codePointAt(i);
            i += Character.charCount(cp);
            if (cp < 0x80) {
                h = step(h, cp);
            } else if (cp < 0x800) {
                h = step(h, 0xC0 | (cp >> 6));
                h = step(h, 0x80 | (cp & 0x3F));
            } else if (cp < 0x10000) {
                h = step(h, 0xE0 | (cp >> 12));
                h = step(h, 0x80 | ((cp >> 6) & 0x3F));
                h = step(h, 0x80 | (cp & 0x3F));
            } else {
                h = step(h, 0xF0 | (cp >> 18));
                h = step(h, 0x80 | ((cp >> 12) & 0x3F));
                h = step(h, 0x80 | ((cp >> 6) & 0x3F));
                h = step(h, 0x80 | (cp & 0x3F));
            }
        }
        return h;
    }

    /** The partition of {@code term} among {@code partitions}: (hash >> 32) mod P. */
    static int of(String term, int partitions) { return (int) ((hash(term) >>> 32) % partitions); }
}
