#!/usr/bin/env bash
# Compiles the shared code and this variant into build/ (docs/design.md, Repository layout,
# rule 6) and records the compiler and JVM versions in build/settings.txt.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
rm -rf "$here/build"
mkdir -p "$here/build"
javac -d "$here/build" "$here"/../_common/*.java "$here"/*.java
echo "javac $(javac -version 2>&1 | cut -d' ' -f2); java $(java -version 2>&1 | head -n 1 | cut -d'"' -f2); run flags: -Xmx20g, default GC" \
    > "$here/build/settings.txt"
