# Concurrent Inverted Index — Benchmark

University project (Concurrent and Parallel Systems): inverted index implementations in
Java, Go, C++ and C, each with several concurrency strategies, benchmarked against each other.
Performance only. Built to portfolio standard: clean, explainable, reproducible.

**`docs/design.md` and `docs/experiment-plan.md` are the source of truth.** `design.md` fixes
the shared infrastructure and rules; `experiment-plan.md` fixes the questions and the
concurrent variants. Read both before any non-trivial change. If something in them looks
wrong or incomplete, say so and ask. Never silently deviate.

## Current phase: Presentation

All three rounds of `docs/experiment-plan.md` are done. Every variant is implemented and
passes `bench/check.sh` at every worker count: `sequential`, `global-lock`, `partition-lock`,
`partition-writers`, `private-merge` and `private-merge-heap` in all four languages,
`partition-rwlock` in Java and C, `private-merge-static` in C. Every experiment has run and is
committed in `results/` (`q1`, `q2`, `q2b`, `q3`, `q4a`, `q4b`, `q5`, without the latency files,
which are kept outside the repository; no GitHub release exists yet); each question has a Results section in the plan.
`results/full/` was a dress rehearsal and is excluded from the analysis (design.md, Layout).

`analysis/make_figures.py` (figures for Q1–Q5 in `analysis/figures/`, Q5 latency percentiles in
`analysis/data/`) is done, and so is the analysis. The README is written for outside readers (the repository is
public). What is left: the presentation, the final deliverable.

- Code shared by variants goes in each language's `_common/`.
- `bench/check.sh` must pass for any new variant or change at every worker
  count before its language is committed. One commit and push per language.

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
  Ask before adding any tool or package. **One exception:** Python, only in `analysis/`, with
  exactly pandas, numpy and matplotlib (pinned in `analysis/requirements.txt`, venv at
  `analysis/.venv`, git-ignored); the benchmark itself keeps this rule (design.md, Analysis plan).
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
