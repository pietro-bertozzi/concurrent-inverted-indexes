#!/usr/bin/env bash
# Starts the Java sequential variant with the CLI contract (docs/design.md, CLI contract).
# The only non-default JVM setting is the fixed heap size (section Build and runtime settings).
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
exec java -Xmx20g -cp "$here/build" Main "$@"
