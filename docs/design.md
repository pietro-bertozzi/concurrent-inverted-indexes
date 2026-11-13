# Concurrent Inverted Index — Benchmark Design

Nov 12, 2026 · Pietro Bertozzi

## Overview

The project compares inverted index implementations across four languages and several concurrency strategies, and measures performance only. It is the individual project for the Concurrent and Parallel Systems course (pass/fail), built to the standard of a portfolio project.

This document fixes the shared benchmark design: repository layout, workload, data, CLI contract, results format, analysis plan and methodology. Each decision carries a short reason. The design of the individual variants (which partitioning and locking combinations to build) comes later, starting from the design space map in *Inverted index — mappa di progetto*.

```
  data/                    ─┐
  corpus prefixes, queries  │
                            │          CLI args
  params.sh                ─┼──▶  bench/run.sh  ─────────▶  <variant>/run.sh
  parameter value lists     │   discover, sweep, ◀─────────  same CLI in every language
                            │       collect        TSV rows
  impl/<lang>/<variant>/   ─┘          │
  found by scanning folders            ▼
                                  results.tsv  ──────────▶  Python analysis
                             rows + context columns         speedup, efficiency, latency
```

Implementations know nothing about each other or the benchmark: the runner reads folders and parameters, calls each variant through the CLI contract, and adds context to the TSV rows it gets back.

## Scope and non-goals

The benchmark measures how fast each implementation builds and queries an inverted index on one machine, with all data in RAM.

**In scope**

- Languages: Java, Go, C++, C. *Why: four very different concurrency models (managed runtime with JIT, goroutines and channels, atomics with zero-cost abstractions, raw pthreads).*
- Extensibility: adding a language or a variant must not require changing any shared tooling. *Why: the project should outlive the exam.*
- Five workload modes: build, query, and three read/write mixes (section Workload model).
- Boolean queries: single term, AND, OR.

**Non-goals**

- Accuracy metrics. *Why: an inverted index has no accuracy; it is either correct or not, and correctness is a gate, not a metric.*
- Ranked retrieval (scoring, BM25).
- Indexes larger than RAM, on-disk storage, multiple machines. *Why: a different problem (see the design space map).*
- Positional indexes, stemming, lemmatization, stopword removal.

## Repository layout and discovery

The filesystem is the registry: the runner discovers every implementation by scanning folders, and no list of languages or variants exists anywhere. *Why: nothing to keep in sync, so nothing can drift from reality.*

```
bench/
  run.sh            runner: discovers, builds, sweeps, collects
  common.sh         shared by run.sh and check.sh: discovery, dimensions, column names
  params.sh         numeric parameters of the main experiment (lists of values)
  params-pinned.sh  control experiment pinned to P-cores (smaller sweep)
  params-quick.sh   small sweep for development
corpus/             preprocessing scripts (raw Wikipedia → corpus, queries)
data/               generated corpus prefixes and query files
impl/
  go/
    _common/        shared Go code (skipped by the runner)
    sequential/     a variant
      build.sh
      run.sh
      dimensions.tsv
    partition-lock/
      ...
  java/  cpp/  c/   same structure
results/            one folder per experiment
analysis/           Python notebooks and scripts (later)
```

**Rules**

1. Every folder `impl/<language>/<variant>/` is one variant of one language. *Why: adding Rust means adding `impl/rust/`; nothing else changes.*
2. Folders starting with `_` hold shared code and are not variants. *Why: input parsing, timing, output and dumping are written once per language, so a fix reaches every variant and the comparison stays fair.*
3. Every variant has two executable entry points with fixed names: `build.sh` (compile this variant) and `run.sh` (start it with the CLI contract). *Why: language-specific knowledge stays in the language folder; the runner calls the same two commands for every language.*
4. Every variant has a `dimensions.tsv` describing itself (section Dimensions). *Why: the description lives next to the code it describes.*
5. Every language has a `sequential` variant. *Why: the baseline for absolute speedup and concurrency overhead.* At discovery, the runner warns for every language folder without a usable `sequential` variant; it does not stop.
6. `build.sh` writes all its outputs into `build/` inside the variant folder, and `run.sh` runs from there. The runner always passes absolute paths (input, queries, latencies, dump) and starts `run.sh` by absolute path; `run.sh` locates `build/` from its own location (`$(dirname "$0")`) and never depends on the working directory. *Why: one ignore rule covers every language, nothing generated ever mixes with source, and a variant behaves the same however and from wherever it is started.*
7. A variant folder without an executable `build.sh`, an executable `run.sh` or a `dimensions.tsv` is skipped with a warning and recorded in `failures.tsv` (phase `discover`), like a failed build. *Why: one half-written variant must not stop an overnight sweep.*

## Dimensions

The experiment space has two kinds of dimensions: **structural** choices change the code and define a variant; **numeric** parameters change without touching the code and live in `bench/params.sh`.

### Structural dimensions (one folder per combination)

From the design space map. Detailed design of each comes later.

| Key | Options | Applies when |
| --- | --- | --- |
| `partitioning` | single · by-term · by-document | always |
| `writer` | direct · central writer · writer per partition | always |
| `coordination` | lock · CAS / lock-free | always; protects the index with direct writes, the queue with a dedicated writer |
| `granularity` | index · partition · posting list | direct writes with locks |
| `write_timing` | immediate · deferred | always |
| `visibility` | partial · snapshot per document · snapshot per batch | mixed modes |

The options form a tree, not a grid: many combinations do not exist, so the real number of variants is far smaller than the product.

Each variant declares its choices in `dimensions.tsv`, one `key<TAB>value` line per dimension, plus which numeric parameters and modes it supports:

```
partitioning	by-term
writer	direct
coordination	lock
granularity	partition
uses	partitions,batch
modes	build,query,mixed-95-5,mixed-50-50,mixed-20-80
```

- A missing key means "not applicable"; the runner writes `-`. *Why: a new dimension is added only to the variants it concerns, and old files stay valid.*
- Key/value lines, not one wide row. *Why: adding a dimension never touches existing files.*
- The runner sweeps only the parameters in `uses` and runs only the modes in `modes`. *Why: no identical repeated runs, no misleading rows.*
- `max_threads` (optional) is the highest worker count the variant supports; the runner skips larger thread values, in strong and weak scaling. A missing key means no limit. **Sequential variants declare `max_threads 1`**, so they run with one worker on every corpus size (build) or operation count (query, mixed). *Why: sweeping a single-threaded variant over 22 worker counts would repeat identical runs.*
- `uses`, `modes` and `max_threads` are instructions to the runner, not dimensions: they never become result columns. *Why: they describe what to run, not what was run; the result columns already say it (`threads`, `mode`, …).*
- A dimension key must not have the same name as a result column (`threads`, `mode`, `run`, …); the runner stops with an error naming the file and line. *Why: two columns with one name would make the header ambiguous.*

### Numeric parameters (`bench/params.sh`)

| Parameter | Meaning | Example values |
| --- | --- | --- |
| `MODES` | workload mode | build query mixed-95-5 mixed-50-50 mixed-20-80 |
| `THREADS`, `THREADS_WEAK` | number of workers N (strong / weak scaling) | 1 2 4 6 8 12 14 16 22 / 1 2 4 8 16 |
| `CORPUS_BASE_MB`, `CORPORA_MB` | corpus prefixes in MB of text, scaled by build mode only; weak scaling uses base × N | 256 · 512 · … · 4096 |
| `QUERY_CORPUS_MB` | the one corpus (MB, one of `CORPORA_MB`) that query and mixed modes run on | 256, to be sized |
| `SCALING` | how total work is set | strong · weak |
| `PARTITIONS` | partitions, if used | 4 16 64 |
| `BATCH` | postings per lock or message, if used | 1 64 1024 |
| `QUEUE` | queue capacity, if used | to be sized |
| `PINNING`, `PCORES_CPUS` | CPU placement (`none` in `params.sh`, `pcores` in `params-pinned.sh`); CPUs used by `pcores` | none · pcores; 0-11 |
| `OPS_TOTAL`, `OPS_PER_WORKER` | operations in a run: strong / weak | to be sized |
| `PRELOAD_DOCS`, `QUERY_COUNT`, `SEED` | documents preloaded in mixed modes; lines in the query file; base seed | to be sized; to be sized; 42 |
| `COOLDOWN`, `SHUFFLE_SEED` | pause before each run in s; seed of the run-order shuffle (recorded in `meta.txt`) | 10; 1 |
| `EXPERIMENT` | name of the results folder | full · pinned · quick |
| `REPEAT`, `WARMUP` | repetitions per run | 10, 1 |
| `TIMEOUT` | limit per run, as a number with an optional `s`, `m`, `h` or `d` suffix | 30m to start |

The file is plain bash, read with `source`. *Why: no parsing and no dependency; every implementation gets exactly the same values from one place.* Language is not a parameter: the folder says it.

There are three params files with the same variable names: `params.sh` (the main, unpinned experiment), `params-pinned.sh` (the pinned control experiment, see Pinning) and `params-quick.sh` (development). The runner validates the file before doing anything: every variable defined, numbers and lists well-formed, `CORPUS_BASE_MB` and `QUERY_CORPUS_MB` listed in `CORPORA_MB`, and, if weak scaling of build is requested, every size base × N listed too.

## Workload model

Every run has N workers, and each worker performs operations according to the mode: a mode is just the probability that an operation is a query rather than an insert.

| Mode | Query share | Insert share | Starts from | Typical of |
| --- | --- | --- | --- | --- |
| `build` | 0% | 100% | empty index | batch indexing |
| `query` | 100% | 0% | finished index (built first) | serving a fixed index |
| `mixed-95-5` | 95% | 5% | preloaded index | search engine (read-heavy) |
| `mixed-50-50` | 50% | 50% | preloaded index | balanced |
| `mixed-20-80` | 20% | 80% | preloaded index | bulk ingestion with occasional queries |

- **Ratios are per operation, not per thread.** *Why: works at any N, including 1 and odd values; it is the method of the standard YCSB benchmark. Splitting threads into readers and writers breaks at small N.*
- **Mixed modes preload part of the corpus, untimed.** Inserts then take the next documents in order. *Why: queries on an empty index would be artificially fast.*
- **Closed loop:** each worker starts its next operation as soon as the previous one ends. *Why: measures maximum capacity, the right question for comparing strategies. Limitation, stated in the report: closed loop understates tail latency under overload (coordinated omission). An open-loop generator was judged too costly for its benefit.*
- **Strong and weak scaling** in every mode. Strong: fixed total work, more workers. Weak: work proportional to N. *Why: strong scaling shows speedup; weak scaling shows whether larger problems stay manageable.*
  - **Build scales the corpus:** strong scaling runs every size in `CORPORA_MB`; weak scaling runs `CORPUS_BASE_MB` × N.
  - **Query and mixed modes run on one fixed corpus** (`QUERY_CORPUS_MB`) and **scale the operations:** `OPS_TOTAL` in strong scaling, `OPS_PER_WORKER` × N in weak scaling; the runner passes the result as `--ops`. *Why: the work of a query run is the number of operations, and a fixed index keeps what is being queried identical across runs; a second corpus-size axis would multiply the sweep for little insight.*
- **Operations beyond the query count:** if a run performs more operations than the query file has lines, each worker starts at an offset in the query file derived from the seed and its worker number, and cycles through the file. *Why: reproducible, different slices of the same query distribution per worker, and any `--ops` works with one query file.*
- **Insert budget:** a mixed run inserts at most ceil(`--ops` × insert share) documents, so preload + budget must not exceed the number of documents of `QUERY_CORPUS_MB`. The runner checks this for every planned mixed run before starting and stops with an error if it fails. *Why: an overnight run must not die in the middle because the corpus ran out.* How a variant guarantees the budget while the operation kind is a probability is part of the variant design (Open items).

## Data

All implementations read the same preprocessed English Wikipedia, so text handling is decided once and can never differ between languages.

### Corpus

- **Source:** English Wikipedia dump. *Why: realistic document lengths, recognizable, and tens of GB of text, far more than needed.*
- **Preprocessing, done once:** strip wiki markup, lowercase, remove punctuation. Keep stopwords; no stemming or lemmatization. A token is a space-separated string.
- **Tokenization rule** (`corpus/normalize.sh`): every run of characters that are not letters or digits (any script) becomes one space, so `don't` gives `don t`. Combining marks are not letters, so words in scripts that use them are split (Devanagari `हिन्दी` becomes `हिन दी`); accepted, because it is negligible on English Wikipedia and, since the corpus is built once, identical for every language.
- **Dropped lines:** a line with invalid UTF-8, or with no tokens left after normalization, is dropped, so the document ID is the line number of the *output*. Nothing is dropped silently: `normalize.sh` prints the number of kept and dropped lines to stderr.
- **Format:** one document per line, tokens separated by single spaces, UTF-8. The document ID is the line number, starting at 0. *Why: trivial to parse even in C; implementations make no decisions about text.*

```
the cat sat on the mat
the dog chased the cat
```

### Corpus sizes

- Sizes are **prefixes** of the same corpus (the first 256 MB, 512 MB, …), doubled by **token count**, not bytes or documents. *Why: same kind of text at every size; tokens measure the actual work, while document lengths vary widely.*
- Vocabulary grows slower than text (Heaps' law): twice the text gives fewer than twice the distinct terms. Accepted and noted in the report.
- Weak-scaling sizes of build mode are multiples of a base unit (about 256 MB at 1 worker, to be confirmed). Every corpus size the runner uses (`CORPORA_MB`, base × N, `QUERY_CORPUS_MB`) must be a prefix listed in `CORPORA_MB`; `make-prefixes.sh` cuts exactly those.

### Postings

Every variant stores the same posting content and delivers the same final order. *Why: if one variant does less work, the comparison is meaningless.*

- A posting = **document ID + term frequency.** *Why: frequency is what real engines need; positions add complexity without concurrency interest.*
- Posting lists are **sorted by document ID** at the end. Any sorting cost counts in `finalize_ns`.

### Queries

- One query per line, operator in capitals first:

```
SIMPLE cat
AND cat dog
OR mat dog
```

- *Why: terms are always lowercase, so an uppercase word can only be an operator.*
- **Generation:** terms are drawn from the preloaded part of the corpus by picking random word occurrences; the operator is chosen at random; fixed seed. *Why: queries hit real documents; frequent words appear often, as in real searches (picking from the vocabulary would give mostly rare words); every variant gets identical queries.*

## CLI contract

Every variant's `run.sh` accepts the same arguments and produces the same outputs; this contract is the only thing the runner knows about an implementation.

```
./run.sh --mode <mode> --input <corpus> --threads N --repeat R --warmup W
         [--queries <file>] [--ops K] [--preload D] [--seed S]
         [--partitions P] [--batch B] [--queue Q]
         [--latencies <file>] [--dump <file>]
```

| Argument | Modes | Meaning |
| --- | --- | --- |
| `--mode` | all | `build`, `query`, `mixed-95-5`, `mixed-50-50`, `mixed-20-80` |
| `--input` | all | corpus file, one document per line |
| `--threads` | all | number of workers N |
| `--repeat` | all | measured repetitions |
| `--warmup` | all | warm-up repetitions run first; recorded and flagged, never discarded |
| `--queries` | query, mixed | query file |
| `--ops` | query, mixed | total operations in the run; the runner computes it (fixed for strong scaling, per-worker × N for weak), so implementations never know which; beyond the query count, workers cycle through the query file (Workload model) |
| `--preload` | mixed | documents indexed before timing starts |
| `--seed` | query, mixed | base seed; each worker derives its own, and its starting offset in the query file |
| `--partitions`, `--batch`, `--queue` | if in `uses` | numeric parameters |
| `--latencies` | query, mixed | file for raw per-query latencies, written after timing (format: section Query latencies) |
| `--dump` | optional | canonical index dump (section Correctness) |

**Conventions**

- Long argument names only; unknown arguments are errors. *Why: a typo fails loudly instead of being silently ignored.*
- **stdout:** TSV data rows only, no header, one row per repetition (warm-ups included), with the measurement columns in the fixed order of section Results; the runner owns the header and treats a wrong row count or field count as a failed run. **stdout** is never anything else (section Results). **stderr:** logs, progress, debug; saved by the runner, never parsed. *Why: a stray print can never corrupt the results.*
- **Exit code:** 0 on success, non-zero on failure; the runner records failures and continues.
- Repetitions run inside one process. *Why: Java's JIT needs warm-up within the same process.*

## Results

Results are raw TSV, one row per repetition: the implementation reports what it measured, the runner adds the context, and Python derives everything else later.

**Principles**

- **TSV everywhere, no extra dependencies.** New columns are only ever added at the end. *Why: easy to write in C and read in bash; old files stay readable.*
- **Raw values only, one row per repetition;** warm-ups kept with `warmup=1`. *Why: averages and ratios can be recomputed from raw data, never the reverse; the JIT effect stays visible.*
- **Integers, nanoseconds and bytes.** *Why: no floats and no unit confusion between languages.*
- **The header is built dynamically:** the runner takes the union of all dimension keys in all `dimensions.tsv` files (runner keys excluded, section Dimensions). *Why: a new dimension shows up as a column automatically.*

### Context columns (written by the runner)

`timestamp` · `commit` · `host` · `cpu` · `cpu_cores` · `cpu_logical` · `language` · `variant` · one column per dimension key (sorted) · one column per numeric parameter (`mode`, `corpus_mb`, `threads`, `scaling`, `partitions`, `batch`, `queue`, `pinning`, `ops`, `preload`, `seed`) · `build_settings` (compiler/JVM version and flags) · `run` (run number, which names the log and latency files). Parameters that do not apply to a run are `-`.

- `commit` is the short hash, with `-dirty` appended if the working tree had uncommitted changes when the sweep started.
- `cpu_cores` is the number of physical cores (16 on the benchmark machine) and `cpu_logical` the number of hardware threads (22). *Why: a benchmark about thread counts must not blur the two.*
- `corpus_mb` is the corpus size in MB, an integer (`failures.tsv` uses the same column names and values).
- `scaling` is `strong`, `weak` or `strong,weak`: runs that would be started with identical arguments (for example build at one worker on the base corpus, which is the same run in both scalings) are executed once and carry both labels. *Why: no identical repeated runs. The analysis selects a scaling with a "contains" test.*
- `build_settings` comes from the first line of `build/settings.txt`, which `build.sh` may write (`-` if absent). The runner deletes an old `build/settings.txt` before each build, so a stale file is never read.

*Why: each language prints only a few fields; metadata is consistent because one program writes it.*

### Measurement columns (written by the implementation to stdout)

| Column | Unit | Meaning |
| --- | --- | --- |
| `rep` | — | repetition index |
| `warmup` | 0/1 | warm-up flag |
| `load_ns` | ns | reading the input |
| `preload_ns` | ns | preloading (mixed modes) |
| `build_ns` | ns | building the index concurrently (build and query modes) |
| `run_ns` | ns | the query or mixed phase |
| `finalize_ns` | ns | sorting, merging, publishing at the end |
| `ops_query`, `ops_insert` | count | operations performed |
| `docs`, `terms`, `postings` | count | index size; also a sanity check |
| `peak_rss_bytes` | bytes | peak process memory |
| `cpu_user_ns`, `cpu_sys_ns` | ns | CPU time during the repetition |
| `ctx_vol`, `ctx_invol` | count | voluntary / involuntary context switches during the repetition |

Columns that do not apply to a mode are `0`. Phases are separate so that loading can never hide the concurrent part.

### Memory semantics

`peak_rss_bytes` is the **process-wide peak**, so it never decreases across repetitions. Memory analysis uses the first measured repetition. *Why: simple, and garbage not yet collected by Java or Go is a real property of those runtimes, worth showing.* CPU time and context switches are before/after differences around each repetition, so they are exact per repetition.

### Query latencies

Each query's latency is recorded in memory and written to the `--latencies` side file **after** the timer stops. The file is one TSV per run (`latencies/<run>.tsv`), without header, with the columns `rep`, `warmup`, `latency_ns` (integers, as in the measurement columns): one line per query operation of every repetition, warm-ups included and flagged. After a successful query or mixed run the runner checks that the file exists, is not empty and that every line has this shape; otherwise the run is recorded as failed (reason `bad-latencies`). Python computes percentiles (p50, p95, p99, max). *Why: percentiles cannot be combined across repetitions (the average of p99s is not the p99), and writing during timing would disturb it. The cost is disk space, kept in check by sizing `--ops`.*

### Layout

```
results/<experiment>/<timestamp>/
  results.tsv          all rows, one per repetition
  latencies/           raw latency files, one per run
  failures.tsv         failed builds and runs, skipped variants (see below)
  logs/                stderr of every run (<run>.log), build output (build-*.log),
                       stdout of failed runs only (<run>.stdout)
  runlist.tsv          the shuffled list of runs, in execution order
  meta.txt             machine, commit, shuffle seed, preflight state before and after,
                       copy of the params file
```

`failures.tsv` has the columns `run`, `phase`, `reason`, `status`, then `language`, `variant` and the run's parameters (all of them except `seed`) with the same names and values as in `results.tsv`. `phase` is `discover` (variant skipped), `build` or `run`. `reason` is a short text for `discover` (for example `run.sh missing or not executable`), `build-failed` for `build`, and for runs `exit`, `timeout`, `bad-output` (wrong row or field count) or `bad-latencies`. `status` is the exit code (124 or 137 for a timeout), `-` if there is none. `run` and the parameters are `-` where they do not apply (discover, build). The stdout of a successful run is deleted after its rows have been appended to `results.tsv`; that of a failed run is kept for diagnosis.

`meta.txt` records the preflight state (power source, swap in use, swap pages moved, platform profile, CPU governor and energy preference) before the sweep and again after it, the `SHUFFLE_SEED`, the physical and logical CPU counts, and a copy of the params file.

## Analysis plan

Python reads the final `results.tsv` and the latency files and computes the standard concurrent-performance metrics; nothing below is computed by the implementations. T(p) is the time with p workers.

| Metric | How it is computed | What it reveals |
| --- | --- | --- |
| Execution time | `build_ns`, `run_ns`, phases | the raw basis of everything else |
| Relative speedup | T(1) / T(p), same variant | how well a variant scales |
| Absolute speedup | T(sequential) / T(p) | whether parallelism actually paid off |
| Efficiency | speedup / p | how well each worker is used; where it drops is the discussion |
| Concurrency overhead | variant at p = 1 vs sequential | the cost of synchronization with no contention |
| Strong scaling | fixed corpus or ops, growing p | speedup curve |
| Weak scaling | work proportional to p | whether time stays flat as the problem grows |
| Throughput | postings/s, queries/s, ops/s | comparable across corpus sizes |
| Serial fraction (Karp–Flatt) | from measured speedup | growing with p means contention or overhead, not inherent serial work |
| CPU utilization | (user + sys) / (wall × p) | low: workers wait; high with no speedup: workers spin (e.g. CAS retries) |
| Context switches | `ctx_vol`, `ctx_invol` | voluntary: blocking on locks; involuntary: preemption, oversubscription |
| Memory | `peak_rss_bytes`, bytes per posting | speed bought with memory |
| Variability | std. deviation, coefficient of variation | predictability; lock-heavy variants are often less stable |
| Latency | p50, p95, p99, max from raw latencies | typical and tail query cost (query and mixed modes) |

Plots mark the hardware boundaries (6, 14, 16, 22 workers) so that hardware effects are never mistaken for algorithm effects.

Hardware counters (cache misses, false sharing via `perf`) are out of scope for now: insightful but platform-specific and an extra tool. A possible later deep dive.

## Correctness

Correctness checking is an optional sanity check, not a trusted guarantee: the canonical dump shows whether variants agree, not that they are right.

With `--dump`, a variant writes its final index in one fixed text form, in a separate untimed run: terms sorted, one per line, postings `docid:frequency` sorted by document ID.

```
cat	0:1 1:1
chased	1:1
dog	1:1
mat	0:1
on	0:1
sat	0:1
the	0:2 1:2
```

- Two variants that built the same index produce byte-identical files, compared with `cmp`. *Why: internally the variants look completely different; only a canonical form makes them comparable.*
- It works for `build` and `query` modes, and for the **final index** of mixed modes: with fixed seeds, the set of inserted documents is deterministic.
- It is the only check that would catch a variant that is fast because it loses updates, the typical race-condition bug. Worth running at least once per variant before trusting its numbers.
- Query results during mixed runs depend on timing and are not compared. Visibility checks (no half-indexed document seen) belong to the variant design.

## Benchmark runner

`bench/run.sh` is a pure bash script that discovers variants from the filesystem, sweeps the parameters in a params file, and collects one `results.tsv` per experiment.

```
./bench/run.sh bench/params.sh          main experiment (unpinned)
./bench/run.sh bench/params-pinned.sh   control experiment pinned to P-cores
./bench/run.sh bench/params-quick.sh    small sweep for development
```

**What it does, in order**

1. `source` the params file and validate it (section Numeric parameters).
2. Discover variants: every `impl/*/*/` folder not starting with `_`; incomplete ones are skipped with a warning (rule 7); a language without a `sequential` variant gets a warning (rule 5).
3. Read every `dimensions.tsv`; build the TSV header from the union of the dimension keys. A name collision with a result column stops the runner.
4. Record the preflight state (power source, swap, CPU profile) in `meta.txt`; warn (do not stop) on battery power.
5. Run each variant's `build.sh`; a failed build is logged and the variant skipped.
6. Build the list of runs: variant × mode (from `modes`) × corpus × threads (up to `max_threads`) × the parameters in `uses` × pinning × scaling. Build scales the corpus, query and mixed use `QUERY_CORPUS_MB`. Compute `--ops` for each. Merge runs with identical arguments, which differ only in scaling, into one run with `scaling` `strong,weak`. Check that every data file exists and that the mixed runs fit in the corpus (insert budget); stop with an error if not.
7. **Shuffle the list**, with a pseudo-random order that depends only on `SHUFFLE_SEED` (a small seeded generator inside `run.sh`; the same params file gives the same order, and the seed is in `meta.txt`). *Why: a laptop heats up over a session; random order keeps throttling from always hitting the same variant. A fixed seed keeps the sweep reproducible.*
8. For each run: optional cool-down pause, then `run.sh` under `timeout`, prefixed with `taskset` when pinned. Check the output, prepend context columns to each stdout row and append to `results.tsv`; save stderr to `logs/`.
9. A non-zero exit, a timeout, malformed output or a malformed latency file is recorded in `failures.tsv` as a failed run, and the runner continues. When pinned to P-cores, runs with more workers than P-core threads are skipped.
10. After the sweep, record the preflight state again in `meta.txt`; warn (do not stop) on battery power or if swap pages were moved during the sweep.

**Rules**

- **Timeouts are generous:** 30 minutes per run to start, tightened later to several times the slowest normal run. *Why: a deadlock would otherwise hang an overnight run forever; standard `timeout` adds no dependency.*
- **Build and runtime settings are fixed and recorded:** compiler optimization flags in `build.sh`, JVM heap (`-Xmx`) and GC flags in `run.sh`, `GOMAXPROCS` set explicitly. *Why: fairness depends on how code is compiled and run, not just on the code.*
- **No dependencies beyond bash and standard Linux tools:** coreutils (including `timeout`), `awk`, `sed`, `taskset` (util-linux), `cmp` (diffutils) and `git` (only for the `commit` column; `unknown` without it).

## Hardware and methodology

The benchmark machine is a laptop with a hybrid CPU, so the design separates hardware effects from algorithm effects explicitly.

### Machine

Lenovo IdeaPad Slim, Intel Core Ultra 7 155H, 31 GB RAM, Linux.

| Core type | Cores | Threads | Speed |
| --- | --- | --- | --- |
| P-cores (performance) | 6 | 12 (hyperthreading) | fast |
| E-cores (efficiency) | 8 | 8 | slower |
| LP E-cores (low power) | 2 | 2 | slowest |
| **Total** | **16** | **22** | |

Adding a worker does not add a fixed amount of compute: speedup curves bend where a core type runs out. The exact CPU numbers of each type are still to be read from `/sys/devices/cpu_core/cpus` and `/sys/devices/cpu_atom/cpus`.

### Thread values

- Strong scaling: `1 2 4 6 8 12 14 16 22`. *Why: includes the hardware boundaries, which are marked on every plot.*
- Weak scaling: `1 2 4 8 16`. *Why: doubling, to match the corpus doubling.*

### Pinning

- **Main experiment unpinned** (`params.sh`, `PINNING=none`): the OS schedules freely, as real software runs.
- **Control experiment pinned to P-cores** (`taskset`), as a separate, smaller experiment: `bench/params-pinned.sh`, with its own `EXPERIMENT` name (`pinned`) and results folder. It runs strong scaling only, workers up to 12 (the P-core threads), the modes build, query and mixed-50-50, two corpus sizes for build, and one value for each numeric parameter; every value is also in `params.sh`, so rows compare directly. *Why: if the bend at 6 workers disappears when pinned, it is proven to be the hardware, not contention. Turns a guess into evidence, without doubling the main sweep.*

### Memory budget

- About **20 GB** for the benchmark; **swap must never be used.** *Why: swapping would dominate the measurements.*
- Estimated index memory: about 1.5× the text size for compact C arrays, up to about 5× for Java or linked lists. The worst variant sets the limit.
- Largest corpus prefix ≈ **4 GB** of text; weak-scaling base ≈ **256 MB** (≈ 40–50 million tokens). Estimates, to be confirmed with the first measured bytes per posting.
- Mixed modes need enough documents after the preloaded part for all inserts; sized together with `--ops` and checked by the runner before the sweep (section Workload model).

### Run conditions

- Plugged in, performance power profile, other programs closed. The runner records the power source, swap use and CPU power profile (platform profile, governor, energy preference) in `meta.txt` before and after the sweep, and warns, without stopping, on battery power or if swap pages were moved during the sweep. The profile is recorded, not enforced; any warning is reported with the results.
- Cool-down pauses between runs and randomized run order. *Why: thin laptops throttle when hot.*
- Per-repetition rows make throttling visible as variability.

## Open items

The shared design is complete; what remains are the variant designs and numbers that need the first measurements.

- [ ] Design the variants: which combinations of structural dimensions to implement, per language, and the question each one answers.
- [ ] Visibility check for mixed modes (what a query may observe).
- [ ] Preprocessing details: the tool for stripping wiki markup is still open (`corpus/normalize.sh` starts from plain text, one document per line). Implemented: tokenization rule and dropped-line counts (section Corpus), prefix cutting (`corpus/make-prefixes.sh`), query generator (`corpus/make-queries.sh`).
- [x] Read the P-core and E-core CPU numbers for pinning: P-cores (with HT) are CPUs 0-11, E and LP E-cores are 12-21 (`/sys/devices/cpu_core/cpus`, `cpu_atom/cpus`).
- [ ] Confirm corpus sizes and the memory budget from measured bytes per posting.
- [ ] Size `--ops`, `--preload`, `QUERY_CORPUS_MB` and the query count so that runs and latency files stay manageable. The runner refuses configurations where preload + insert budget exceed the documents of `QUERY_CORPUS_MB`, so the current placeholders in `params.sh` will not pass on the real corpus until sized.
- [ ] Insert budget in mixed modes: how a variant guarantees at most ceil(`--ops` × insert share) inserts although the operation kind is a probability (for example a per-worker pattern or quota instead of random draws).
- [ ] Baseline for weak scaling: a sequential variant (`max_threads 1`) runs build on every corpus size (strong rows) but query and mixed only with the fixed `OPS_TOTAL` or `OPS_PER_WORKER`; decide how absolute speedup is computed for weak query and mixed runs.
- [ ] Fix compiler flags, JVM heap and GC settings, `GOMAXPROCS`.
- [ ] Tighten `TIMEOUT` after the first full runs.
- [x] Decide how `results/<experiment>` folders are named: each params file sets `EXPERIMENT` (`full`, `pinned`, `quick`), so dev results land in ignored folders.
