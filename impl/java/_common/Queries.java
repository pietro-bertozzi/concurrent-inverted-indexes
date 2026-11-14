import java.io.BufferedReader;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;

/** The query file (docs/design.md, section Queries), parsed once into arrays: "SIMPLE t", "AND a b", "OR a b". */
final class Queries {
    static final int SIMPLE = 0, AND = 1, OR = 2;

    final int[] kind;
    final String[] first, second;   // second is null for SIMPLE
    final int size;

    private Queries(List<int[]> kinds, List<String[]> terms) {
        size = kinds.size();
        kind = new int[size];
        first = new String[size];
        second = new String[size];
        for (int i = 0; i < size; i++) {
            kind[i] = kinds.get(i)[0];
            first[i] = terms.get(i)[0];
            second[i] = terms.get(i)[1];
        }
    }

    /** Reads and parses {@code file}; throws IOException with the line number on a malformed line. */
    static Queries load(String file) throws IOException {
        List<int[]> kinds = new ArrayList<>();
        List<String[]> terms = new ArrayList<>();
        try (BufferedReader in = Files.newBufferedReader(Path.of(file), StandardCharsets.UTF_8)) {
            String line;
            int number = 0;
            while ((line = in.readLine()) != null) {
                number++;
                String[] f = line.split(" ");
                int k;
                switch (f[0]) {
                    case "SIMPLE" -> k = SIMPLE;
                    case "AND" -> k = AND;
                    case "OR" -> k = OR;
                    default -> throw new IOException(file + " line " + number + ": unknown operator '" + f[0] + "'");
                }
                if (f.length != (k == SIMPLE ? 2 : 3)) {
                    throw new IOException(file + " line " + number + ": wrong number of terms for " + f[0]);
                }
                kinds.add(new int[] {k});
                terms.add(new String[] {f[1], k == SIMPLE ? null : f[2]});
            }
        }
        if (kinds.isEmpty()) throw new IOException(file + " has no queries");
        return new Queries(kinds, terms);
    }
}
