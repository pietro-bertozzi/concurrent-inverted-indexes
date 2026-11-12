#!/usr/bin/env bash
# Cuts the corpus prefixes (docs/design.md, section Corpus sizes).
#
#   corpus/make-prefixes.sh <params-file> <corpus.txt> [output-dir]
#
# Writes <output-dir>/corpus-<MB>mb.txt (default output-dir: data/) for every size in
# CORPORA_MB of the params file. The smallest size, CORPUS_BASE_MB, is the first whole
# documents that reach that many megabytes (1 MB = 10^6 bytes); its token count is T.
# Every other size is the first whole documents that reach T * size / base tokens, so
# sizes double by token count and each prefix is a prefix of the next.
set -euo pipefail

# Prints an error to stderr and stops.
die() { echo "make-prefixes.sh: error: $*" >&2; exit 1; }

# Checks the arguments and the params file, then cuts all prefixes in one pass.
main() {
    [[ $# -ge 2 && $# -le 3 ]] || die "usage: corpus/make-prefixes.sh <params-file> <corpus.txt> [output-dir]"
    local params="$1" corpus="$2"
    local out="${3:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/data}"
    [[ -f "$params" ]] || die "params file '$params' does not exist"
    [[ -f "$corpus" ]] || die "corpus '$corpus' does not exist"
    mkdir -p "$out"
    # shellcheck source=/dev/null
    source "$params"
    local size
    for size in $CORPORA_MB; do
        if (( size < CORPUS_BASE_MB )); then
            die "CORPORA_MB has $size, smaller than CORPUS_BASE_MB=$CORPUS_BASE_MB; the base must be the smallest size"
        fi
    done
    # LC_ALL=C: length() counts bytes. One pass: all prefixes are written while open;
    # once the base size is reached, token targets are known and each prefix closes when
    # it reaches its own.
    LC_ALL=C awk -v sizes="$CORPORA_MB" -v base_mb="$CORPUS_BASE_MB" -v out="$out" '
        BEGIN {
            n = split(sizes, mb, " ")
            for (i = 1; i <= n; i++) file[i] = out "/corpus-" mb[i] "mb.txt"
            base_bytes = base_mb * 1000000
            open_count = n
        }
        {
            for (i = 1; i <= n; i++) if (!closed[i]) print $0 > file[i]
            bytes += length($0) + 1
            tokens += NF
            if (!base_done && bytes >= base_bytes) {
                base_done = 1
                for (i = 1; i <= n; i++) target[i] = tokens * mb[i] / base_mb
            }
            if (base_done) {
                for (i = 1; i <= n; i++) {
                    if (!closed[i] && tokens >= target[i]) {
                        closed[i] = 1; open_count--; close(file[i])
                        printf "%s: %d tokens, %d bytes, %d documents\n", file[i], tokens, bytes, NR > "/dev/stderr"
                    }
                }
                if (open_count == 0) { finished = 1; exit }
            }
        }
        END {
            if (!finished) {
                printf "make-prefixes.sh: error: corpus too small: %d bytes, %d tokens read, prefixes still open\n", bytes, tokens > "/dev/stderr"
                exit 1
            }
        }
    ' "$corpus"
}

main "$@"
