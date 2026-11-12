# Concurrent Inverted Index — Benchmark

University project (Concurrent and Parallel Systems): inverted index implementations in
Java, Go, C++ and C, each with several concurrency strategies, benchmarked against each other.
Performance only. Built to portfolio standard: clean, explainable, reproducible.

**`docs/design.md` is the source of truth.** Read it before any non-trivial change.
If something in it looks wrong or incomplete, say so and ask. Never silently deviate.

## Current phase: infrastructure

Building the benchmark infrastructure. No index implementations yet; testing happens
later, against the real implementations.

1. Repository skeleton (folders, `.gitignore`, `README.md`)
2. `bench/params.sh` and `bench/params-quick.sh`
3. `bench/run.sh`: discovery → `dimensions.tsv` → build → run list → shuffle → run with
   `timeout`/`taskset` → context columns → `results.tsv`, `logs/`, `meta.txt`
4. Hand-made test data: tiny corpus, query file, expected canonical dump
5. Preprocessing: Wikipedia → corpus, prefixes by token count, query generator

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
- **No dependencies** beyond bash and standard Linux tools (coreutils, `timeout`, `taskset`,
  `shuf`). Ask before adding any tool or package.
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

## Machine

Intel Core Ultra 7 155H (6 P-cores with HT, 8 E-cores, 2 LP E-cores = 16 cores / 22 threads),
31 GB RAM, Linux laptop. Hybrid CPU: see design.md, section Hardware and methodology.
