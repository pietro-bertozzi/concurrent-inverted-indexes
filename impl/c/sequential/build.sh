#!/usr/bin/env bash
# Compiles the shared code and this variant into build/ (docs/design.md, Repository layout,
# rule 6) with gcc -O2 -std=c17 and records the compiler version and flags in
# build/settings.txt. The shared headers are found through -I.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
common="$here/../_common"

rm -rf "$here/build"
mkdir -p "$here/build"
gcc -O2 -std=c17 -I "$common" -I "$here" "$common"/*.c "$here"/*.c -o "$here/build/index"
echo "gcc $(gcc -dumpfullversion); flags: -O2 -std=c17" > "$here/build/settings.txt"
