# Concurrent Inverted Index — Benchmark

University project (Concurrent and Parallel Systems): inverted index implementations in
Java, Go, C++ and C, each with several concurrency strategies, benchmarked against each other.
Performance only. Built to portfolio standard: clean, explainable, reproducible.

**`docs/design.md` and `docs/experiment-plan.md` are the source of truth.** `design.md` fixes
the shared infrastructure and rules; `experiment-plan.md` fixes the questions and the
concurrent variants. Read both before any non-trivial change. If something in them looks
wrong or incomplete, say so and ask. Never silently deviate.

## Current phase: Round 1: global-lock and partition-lock

The infrastructure and the four `sequential` baselines are done and swept
(`results/full/`). Now round 1 of `docs/experiment-plan.md` (section The implementations:
Rules every variant follows, How each variant works, Order of work):

- `global-lock` (`uses` batch) and `partition-lock` (`uses` partitions, batch), all five
  modes, in the order Java, Go, C++, C.
- Code shared by variants (partition hash, sorted insertion, the worker loop with the
  dynamic counter) goes in each language's `_common/`.
- `bench/check.sh` must pass for each new variant at every worker count before its
  language is committed. One commit and push per language.
- Rounds 2 and 3 (`private-merge`, `partition-writers`, extras) come later.

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
