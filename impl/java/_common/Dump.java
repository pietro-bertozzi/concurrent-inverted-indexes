import java.io.BufferedWriter;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Map;

/**
 * The canonical dump (docs/design.md, section Correctness): terms sorted by their UTF-8
 * bytes (the order of `LC_ALL=C sort`), one per line, then a tab and the postings
 * "docid:frequency" sorted by document ID, separated by single spaces.
 */
final class Dump {
    private record Entry(byte[] key, String term, PostingList list) {}

    /** Writes the index to {@code file}. */
    static void write(Index index, String file) throws IOException {
        List<Entry> entries = new ArrayList<>(index.terms());
        for (Map.Entry<String, PostingList> e : index.entries()) {
            entries.add(new Entry(e.getKey().getBytes(StandardCharsets.UTF_8), e.getKey(), e.getValue()));
        }
        entries.sort((x, y) -> Arrays.compareUnsigned(x.key, y.key));
        try (BufferedWriter out = Files.newBufferedWriter(Path.of(file), StandardCharsets.UTF_8)) {
            StringBuilder line = new StringBuilder();
            for (Entry e : entries) {
                line.setLength(0);
                line.append(e.term).append('\t');
                for (int i = 0; i < e.list.size; i++) {
                    if (i > 0) line.append(' ');
                    line.append(e.list.docs[i]).append(':').append(e.list.freqs[i]);
                }
                line.append('\n');
                out.append(line);
            }
        }
    }
}
