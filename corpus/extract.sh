#!/usr/bin/env bash
# Raw pre-extracted Wikipedia text -> one document per line (docs/design.md, section Corpus).
#
#   zcat data/raw/wikipedia.txt.gz | corpus/extract.sh > plain.txt
#
# Input: the TU Darmstadt English Wikipedia corpus (WikiExtractor output without <doc>
# tags): per article a title line, one empty line, then the body, one paragraph or heading
# per line; articles are separated by two or more empty lines.
# Output: one document per line (title and body joined with spaces), HTML-like tags such as
# <br> or </ref> removed; case and punctuation are left to normalize.sh.
#
# Article boundary: a non-empty line (the title) that is preceded by at least two empty
# lines (or starts the file), followed by exactly one empty line and then text. Runs of
# empty lines alone are not enough: sections whose content was stripped (for example
# "External links") also leave runs of two or more empty lines inside an article.
# Streaming: lines are examined two at a time late, so the memory is one article.
set -euo pipefail

# C locale: awk works on bytes, so invalid UTF-8 passes through untouched (normalize.sh
# is the step that drops such lines).
export LC_ALL=C

main() {
    awk '
        # Adds one body line to the current document: tags removed, joined with a space.
        function add(line) {
            gsub(/<[^<>]*>/, " ", line)
            doc = doc " " line
        }
        # Prints the current document (if there is one) as a single line.
        function emit() {
            if (doc != "") { sub(/^ /, "", doc); print doc; n++ }
            doc = ""
        }
        # Decides whether line j is a title, once lines j+1 and j+2 have been read.
        function is_title(j) {
            if (buf[j] == "" || buf[j + 1] != "" || buf[j + 2] == "") return 0
            if (j == 1) return 1
            return j >= 3 && buf[j - 1] == "" && buf[j - 2] == ""
        }
        # Handles line j: a title starts a new document, anything else is body text.
        function handle(j) {
            if (is_title(j))   { emit(); add(buf[j]) }
            else if (buf[j] != "") add(buf[j])
            delete buf[j - 2]
        }
        { buf[NR] = $0 }
        NR >= 3 { handle(NR - 2) }
        END {
            # The last two lines cannot be titles: they have no two lines after them.
            for (j = (NR >= 2 ? NR - 1 : 1); j <= NR; j++) if (j >= 1 && j in buf && buf[j] != "") add(buf[j])
            emit()
            printf "extract.sh: wrote %d documents\n", n > "/dev/stderr"
        }'
}

main
