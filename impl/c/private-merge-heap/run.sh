#!/usr/bin/env bash
# Starts the C private-merge-heap variant with the CLI contract (docs/design.md, CLI contract).
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
exec "$here/build/index" "$@"
