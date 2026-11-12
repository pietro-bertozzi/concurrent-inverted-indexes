#!/usr/bin/env bash
# Generates the query file (docs/design.md, section Queries).
#
#   corpus/make-queries.sh <params-file> <corpus.txt> [output-file]
#
# Writes QUERY_COUNT queries (default output: data/queries.txt). Each query gets a random
# operator (SIMPLE: one term, AND/OR: two terms), and every term is a randomly chosen word
# occurrence in the first PRELOAD_DOCS documents of the corpus, so frequent words are
# frequent in queries too. The generator is seeded with SEED and uses its own
# integer generator, so the output does not depend on the awk implementation.
# Corpus prefixes share their first documents, so any prefix gives the same queries.
set -euo pipefail

# Prints an error to stderr and stops.
die() { echo "make-queries.sh: error: $*" >&2; exit 1; }

# Checks the arguments, then picks the word occurrences in two passes over the head of the corpus.
main() {
    [[ $# -ge 2 && $# -le 3 ]] || die "usage: corpus/make-queries.sh <params-file> <corpus.txt> [output-file]"
    local params="$1" corpus="$2"
    local output="${3:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/data/queries.txt}"
    [[ -f "$params" ]] || die "params file '$params' does not exist"
    [[ -f "$corpus" ]] || die "corpus '$corpus' does not exist"
    # shellcheck source=/dev/null
    source "$params"
    [[ -n "${QUERY_COUNT:-}" && -n "${PRELOAD_DOCS:-}" && -n "${SEED:-}" ]] \
        || die "$params must define QUERY_COUNT, PRELOAD_DOCS and SEED"

    local head_file
    head_file="$(mktemp)"
    trap "rm -f '$head_file'" EXIT
    head -n "$PRELOAD_DOCS" "$corpus" > "$head_file"
    mkdir -p "$(dirname "$output")"

    # The file is given twice: pass 1 counts the word occurrences; at the start of pass 2
    # the random occurrences are chosen, and pass 2 collects those words.
    LC_ALL=C awk -v count="$QUERY_COUNT" -v seed="$SEED" '
        # Park-Miller generator: state * 16807 stays below 2^53, so doubles are exact.
        function random(n) {
            state = (state * 16807) % 2147483647
            return int((state - 1) / 2147483646 * n)
        }
        FNR == NR { total += NF; next }
        FNR == 1 {
            state = seed % 2147483647; if (state == 0) state = 1
            split("SIMPLE AND OR", operators, " ")
            for (q = 1; q <= count; q++) {
                op[q] = operators[random(3) + 1]
                terms[q] = (op[q] == "SIMPLE") ? 1 : 2
                for (k = 1; k <= terms[q]; k++) {
                    position = random(total) + 1
                    wanted[position] = wanted[position] " " q "," k
                }
            }
        }
        {
            for (i = 1; i <= NF; i++) {
                seen++
                if (seen in wanted) {
                    m = split(wanted[seen], slots, " ")
                    for (j = 1; j <= m; j++) word[slots[j]] = $i
                }
            }
        }
        END {
            if (total == 0) { print "make-queries.sh: error: no words in the first documents" > "/dev/stderr"; exit 1 }
            for (q = 1; q <= count; q++) {
                line = op[q]
                for (k = 1; k <= terms[q]; k++) line = line " " word[q "," k]
                print line
            }
        }
    ' "$head_file" "$head_file" > "$output"
    echo "make-queries.sh: wrote $QUERY_COUNT queries to $output" >&2
}

main "$@"
