#!/usr/bin/env bash
# Plain text -> corpus format (docs/design.md, section Corpus).
#
#   corpus/normalize.sh < plain.txt > corpus.txt
#
# Input: one document per line, wiki markup already removed.
# Output: one document per line, lowercase, every run of non-letter/non-digit characters
# replaced by a single space, tokens separated by single spaces. Stopwords are kept; no
# stemming. Lines left without tokens are dropped, so line number = document ID.
set -euo pipefail

# C.UTF-8 makes sed treat accented letters and non-Latin scripts as letters.
export LC_ALL=C.UTF-8

# \L& lowercases the line; the next substitutions turn punctuation into spaces and trim.
sed -E 's/.*/\L&/; s/[^[:alnum:]]+/ /g; s/^ +//; s/ +$//' | grep -v '^$' || true
