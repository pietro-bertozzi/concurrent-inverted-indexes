#!/usr/bin/env bash
# Compiles the shared code and this variant into build/ (docs/design.md, Repository layout,
# rule 6) and records the Go version in build/settings.txt. `go build` accepts named files
# from one directory only, so the sources of _common/ and of the variant are first copied
# into build/src/ and compiled from there, as package main, without go.mod.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
common="$here/../_common"

# Stops with an error if a file name exists both in _common/ and in the variant.
check_no_duplicates() {
    local f
    for f in "$here"/*.go; do
        if [[ -e "$common/$(basename "$f")" ]]; then
            echo "error: $(basename "$f") exists in both _common/ and $(basename "$here")/; rename one of them" >&2
            exit 1
        fi
    done
}

check_no_duplicates
rm -rf "$here/build"
mkdir -p "$here/build/src"
cp "$common"/*.go "$here"/*.go "$here/build/src/"
(cd "$here/build/src" && go build -o "$here/build/index" ./*.go)
echo "$(go version | cut -d' ' -f3,4); go build defaults; run: defaults, GOMAXPROCS logged on stderr" \
    > "$here/build/settings.txt"
