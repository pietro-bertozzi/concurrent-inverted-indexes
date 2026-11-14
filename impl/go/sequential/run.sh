#!/usr/bin/env bash
# Starts the Go sequential variant with the CLI contract (docs/design.md, CLI contract).
# No runtime settings are changed; GOMAXPROCS is logged on stderr by the program.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
exec "$here/build/index" "$@"
