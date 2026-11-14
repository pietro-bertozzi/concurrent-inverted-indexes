#!/usr/bin/env bash
# Compiles the shared code and this variant into build/ (docs/design.md, Repository layout,
# rule 6) with g++ -O2 -std=c++17 and records the compiler version and flags in
# build/settings.txt. The shared headers are found through -I.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
common="$here/../_common"

rm -rf "$here/build"
mkdir -p "$here/build"
g++ -O2 -std=c++17 -I "$common" "$common"/*.cpp "$here"/*.cpp -o "$here/build/index"
echo "g++ $(g++ -dumpfullversion); flags: -O2 -std=c++17" > "$here/build/settings.txt"
