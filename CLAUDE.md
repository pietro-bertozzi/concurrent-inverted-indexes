# Concurrent Inverted Index — Benchmark

University project (Concurrent and Parallel Systems): inverted index implementations in
Java, Go, C++ and C, each with several concurrency strategies, benchmarked against each other.
Performance only. Built to portfolio standard: clean, explainable, reproducible.

**`docs/design.md` is the source of truth.** Read it before any non-trivial change.
If something in it looks wrong or incomplete, say so and ask. Never silently deviate.

## Current phase: sequential baselines

Infrastructure is done: skeleton, `bench/params.sh` / `params-pinned.sh` / `params-quick.sh`,
`bench/run.sh`, `bench/check.sh`, `bench/common.sh`, test data in `data/tiny/`, preprocessing
in `corpus/`. Now the four `sequential` variants, following `docs/design.md`, section
Shared implementation rules (splitmix64, exact operation counts, query algorithms,
build/runtime settings, data structures).

1. Infrastructure updates from the new design: the mixed-sizing check in `run.sh` uses
   exactly `preload + floor(ops × insert share / 100)`; `check.sh` checks mixed modes against
   a build-mode reference dump of the corpus prefix; `CHECK_OPS` and `CHECK_PRELOAD` in all
   three params files
2. `impl/java/_common/` and `impl/java/sequential/`: all five modes, latencies, dump,
   measurement columns; first end-to-end test of the runner and `check.sh`
3. `impl/go/_common/` and `impl/go/sequential/`, with Java as the reference
4. `impl/cpp/_common/` and `impl/cpp/sequential/`
5. `impl/c/_common/` (with a hand-written hash map: its design is not decided yet, ask)
   and `impl/c/sequential/`

`bench/check.sh` must pass for every variant before its step is committed.

## Rules that must not be broken without asking

- **No hard-coded lists.** Languages and variants are discovered from `impl/<lang>/<variant>/`.
  Folders starting with `_` are shared code, never variants.
- **Every variant** has executable `build.sh`, `run.sh` and a key/value `dimensions.tsv`
  (keys include `uses` and `modes`). The runner only ever calls these.
- **CLI contract** (design.md, section CLI contract) is fixed. Long options only; unknown
  options are errors.
- **stdout = TSV only. stderr = logs. Exit code = success/failure.** Never print anything
  else to stdout.
- **Results format:** TSV, one row per repetition, raw values, integers in ns and bytes.
  Column order is fixed; new columns are only added at the end. No averaging anywhere
  except in the (later) Python analysis.
- **No dependencies** beyond bash and the standard Linux tools actually used: coreutils
  (including `timeout`), `awk`, `sed`, `taskset` (util-linux), `cmp` (diffutils) and `git`,
  plus the language toolchains (`javac`/`java`, `go`, `g++`, `gcc`). Implementations use
  only their language's standard library: no Maven/Gradle, no third-party modules.
  Ask before adding any tool or package.
- **Identical work in every language:** splitmix64 from `_common/`, never a built-in
  generator; exact operation counts and draw order as in design.md. A deviation here
  silently breaks cross-language comparison.
- **Numeric parameters** live only in `bench/params*.sh` (plain bash, `source`d).

## Code style

- Bash: `#!/usr/bin/env bash`, `set -euo pipefail`, quote every variable, small functions,
  a comment above each function saying what it does. Bash 4+ features are fine.
- Prefer clear over clever. This code must be explainable line by line at an exam and in
  interviews.
- Error messages go to stderr and say what failed and why.

## How I want to work

- Implement everything that is already decided in `docs/design.md` without stopping between
  steps. No plan approval needed.
- Stop and ask only when something is not decided in the design, or would change it.
- After each completed step, stage only the files of that step, commit with a clear message
  (what and why), and push.
- Don't touch files outside the current step.
- `docs/design.md` must always match the code. Every decision, deviation or new
  convention updates `design.md` in the same commit as the code that implements it.
  `README.md` stays in sync with how to run things. Never leave a decision only in the
  chat or only in the code.

## Machine

Intel Core Ultra 7 155H (6 P-cores with HT, 8 E-cores, 2 LP E-cores = 16 cores / 22 threads),
31 GB RAM, Linux laptop. Hybrid CPU: see design.md, section Hardware and methodology.
