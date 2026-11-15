#!/usr/bin/env bash
# Starts the Java partition-rwlock variant with the CLI contract (docs/design.md, CLI contract).
# The only non-default JVM settings are the fixed heap size and the permission for native calls
# (getrusage through the FFM API); see docs/design.md, section Build and runtime settings.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
exec java -Xmx20g --enable-native-access=ALL-UNNAMED -cp "$here/build" Main "$@"
