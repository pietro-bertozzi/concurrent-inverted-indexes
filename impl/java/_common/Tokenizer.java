/**
 * Splits a document line into tokens. The corpus has single spaces between tokens and no
 * empty tokens (docs/design.md, section Data), so a token is just the text between spaces.
 */
final class Tokenizer {
    private final String doc;
    private int start = 0;
    private String token;

    Tokenizer(String doc) { this.doc = doc; }

    /** Moves to the next token; returns false when there are no more. */
    boolean next() {
        if (start >= doc.length()) return false;
        int end = doc.indexOf(' ', start);
        if (end < 0) end = doc.length();
        token = doc.substring(start, end);
        start = end + 1;
        return true;
    }

    /** The current token (valid after next() returned true). */
    String token() { return token; }
}
