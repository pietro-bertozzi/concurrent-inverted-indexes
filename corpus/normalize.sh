#!/usr/bin/env bash
# Plain text -> corpus format (docs/design.md, section Corpus).
#
#   corpus/normalize.sh < plain.txt > corpus.txt
#
# Input: one document per line, wiki markup already removed.
# Output: one document per line, lowercase, every run of non-letter/non-digit characters
# replaced by a single space, tokens separated by single spaces. Stopwords are kept; no
# stemming. Lines that cannot become documents are dropped, so line number = document ID.
# Nothing is dropped silently: the number of kept lines and of dropped lines (invalid
# UTF-8, or no tokens left) is printed to stderr at the end.
set -euo pipefail

# C.UTF-8 makes sed treat accented letters and non-Latin scripts as letters.
export LC_ALL=C.UTF-8

# In a UTF-8 locale `.` does not match an invalid byte, so sed can recognise a line with
# invalid UTF-8 (`/^.*$/!`) and mark it with a leading \x01 instead of normalizing it.
# Valid lines: \L& lowercases, then punctuation becomes spaces, then trim.
# awk (byte mode) drops the marked lines and the empty ones and counts both.
main() {
    sed -E '/^.*$/!{s/^/\x01/;b};s/.*/\L&/; s/[^[:alnum:]]+/ /g; s/^ +//; s/ +$//' \
        | LC_ALL=C awk '
            substr($0, 1, 1) == "\001" { invalid++; next }
            $0 == ""                   { empty++;   next }
                                       { kept++; print }
            END {
                printf "normalize.sh: kept %d documents; dropped %d lines with invalid UTF-8 and %d lines without tokens\n",
                       kept, invalid, empty > "/dev/stderr"
            }'
}

main
