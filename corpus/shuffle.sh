#!/usr/bin/env bash
# Shuffles the documents once, with a fixed seed (docs/design.md, section Corpus).
#
#   corpus/shuffle.sh <params-file> < ordered.txt > shuffled.txt
#
# Every line (document) gets a random key from a generator seeded with CORPUS_SEED of the
# params file; `sort` orders the lines by key. Same input and seed give the same output.
# The generator is our own integer one (as in make-queries.sh), so the result does not
# depend on the awk implementation. Temporary sort files go to $TMPDIR (default /tmp):
# the sort of an 11 GB corpus needs about as much free space there as the corpus itself.
set -euo pipefail

# Prints an error to stderr and stops.
die() { echo "shuffle.sh: error: $*" >&2; exit 1; }

# Checks the arguments, then prefixes each line with its key, sorts by key and removes the key.
main() {
    [[ $# -eq 1 ]] || die "usage: corpus/shuffle.sh <params-file> < ordered.txt > shuffled.txt"
    local params="$1"
    [[ -f "$params" ]] || die "params file '$params' does not exist"
    # shellcheck source=/dev/null
    source "$params"
    [[ -n "${CORPUS_SEED:-}" ]] || die "$params must define CORPUS_SEED"

    # Park-Miller generator: state * 16807 stays below 2^53, so doubles are exact. Its states
    # are all different for 2^31-2 steps, so no two lines get the same key (no ties).
    # The key is zero-padded, so the plain byte-wise sort (LC_ALL=C) orders it numerically.
    LC_ALL=C awk -v seed="$CORPUS_SEED" '
        BEGIN { state = seed % 2147483647; if (state == 0) state = 1 }
        {
            state = (state * 16807) % 2147483647
            printf "%010d\t%s\n", state, $0
        }' \
        | LC_ALL=C sort -t $'\t' -k1,1 \
        | cut -f2-
}

main "$@"
