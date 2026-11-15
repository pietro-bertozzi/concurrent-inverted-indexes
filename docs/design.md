# Concurrent Inverted Index — Benchmark Design

Nov 14, 2026 · Pietro Bertozzi

## Overview

The project compares inverted index implementations across four languages and several concurrency strategies, and measures performance only. It is the individual project for the Concurrent and Parallel Systems course (pass/fail), built to the standard of a portfolio project.

This document fixes the shared benchmark design: repository layout, workload, data, CLI contract, results format, analysis plan and methodology. Each decision carries a short reason. The questions the benchmark answers and the concurrent variants built to answer them are in `docs/experiment-plan.md`, which is part of the source of truth together with this document.

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
  check.sh          correctness check: every variant against the expected dump
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
5. Every language has a `sequential` variant. *Why: the baseline for absolute speedup and concurrency overhead.* At discovery, the runner (and `check.sh`) warns for every language folder without a usable `sequential` variant; it does not stop.
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
- `max_threads` (optional) is the highest worker count the variant supports. A missing key means no limit. In strong scaling the runner skips larger thread values. In weak scaling it does not skip them: a variant still runs the work of N (the same corpus or `--ops` as the N-worker run) with `max_threads` workers, and the `work_n` column records N (section Workload model). **Sequential variants declare `max_threads 1`**, so they run on every corpus size and operation count with one worker. *Why: sweeping a single-threaded variant over 22 worker counts in strong scaling would repeat identical runs, while weak scaling needs the one-worker time of every N's work as its baseline.*
- `uses`, `modes` and `max_threads` are instructions to the runner, not dimensions: they never become result columns. *Why: they describe what to run, not what was run; the result columns already say it (`threads`, `mode`, …).*
- A dimension key must not have the same name as a result column (`threads`, `mode`, `run`, …); the runner stops with an error naming the file and line. *Why: two columns with one name would make the header ambiguous.*

### Numeric parameters (`bench/params.sh`)

| Parameter | Meaning | Example values |
| --- | --- | --- |
| `MODES` | workload mode | build query mixed-95-5 mixed-50-50 mixed-20-80 |
| `THREADS`, `THREADS_WEAK` | number of workers N (strong / weak scaling) | 1 2 4 6 8 12 14 16 22 / 1 2 4 8 16 |
| `CORPUS_BASE_MB`, `CORPORA_MB` | corpus prefixes in MB of text, scaled by build mode only; weak scaling uses base × N | 128 · 256 · … · 2048 |
| `QUERY_CORPUS_MB` | the one corpus (MB, one of `CORPORA_MB`) that query and mixed modes run on | 512 |
| `SCALING` | how total work is set | strong · weak |
| `PARTITIONS` | partitions, if used | 4 16 64 |
| `BATCH` | postings per lock or message, if used | 1 64 1024 |
| `QUEUE` | queue capacity, if used | to be sized |
| `PINNING`, `PCORES_CPUS` | CPU placement (`none` in `params.sh`, `pcores` in `params-pinned.sh`); CPUs used by `pcores` | none · pcores; 0-11 |
| `OPS_TOTAL`, `OPS_PER_WORKER` | operations in a run: strong / weak | 100,000 / 5,000 |
| `PRELOAD_DOCS`, `QUERY_COUNT`, `SEED` | documents preloaded in mixed modes; lines in the query file; base seed | 100,000; 100,000; 42 |
| `CORPUS_SEED` | seed of the one-time document shuffle (`corpus/shuffle.sh`); used by preprocessing only, not by the runner | 2015 |
| `COOLDOWN`, `SHUFFLE_SEED` | pause before each run in s; seed of the run-order shuffle (recorded in `meta.txt`) | 10; 1 |
| `EXPERIMENT` | name of the results folder | full · pinned · quick |
| `CHECK_REPEAT`, `CHECK_WARMUP`, `CHECK_PARTITIONS`, `CHECK_BATCH`, `CHECK_QUEUE` | run shape and numeric parameters `check.sh` passes to variants (read only by `check.sh`) | 1, 0; 4, 2, 8 |
| `CHECK_OPS`, `CHECK_PRELOAD` | `--ops` and `--preload` that `check.sh` passes in query and mixed modes; preload + inserts of the largest insert share must fit in the documents of `data/tiny/` | 7, 1 |
| `REPEAT`, `WARMUP` | repetitions per run | 10, 1 |
| `TIMEOUT` | limit per run, as a number with an optional `s`, `m`, `h` or `d` suffix | 30m to start |

**`CHECK_OPS=7`, `CHECK_PRELOAD=1` (all three params files).** `data/tiny/` has 6 documents and 7 queries, and three wishes cannot all hold: preload + floor(`CHECK_OPS` × 80 / 100) ≤ 6, `CHECK_OPS` > 7, and a preload above 0. With `CHECK_OPS` ≥ 8 the largest share already inserts 6 documents, which forces a preload of 0. *Why 7 and 1: wrap-around of the query file is still exercised, because every worker starts at a random offset and cycles past the end; a preload of 1 keeps the preload path tested (1 + floor(7 × 80 / 100) = 1 + 5 = 6 documents, exactly the corpus).*

With `CHECK_OPS=7`, mixed-95-5 does floor(7 × 5 / 100) = 0 inserts, so inserts are checked only by mixed-50-50 and mixed-20-80; mixed-95-5 checks the preload and the queries.

The file is plain bash, read with `source`. *Why: no parsing and no dependency; every implementation gets exactly the same values from one place.* Language is not a parameter: the folder says it.

There are three params files with the same variable names: `params.sh` (the main, unpinned experiment), `params-pinned.sh` (the pinned control experiment, see Pinning) and `params-quick.sh` (development). The runner validates the file before doing anything: every variable defined, numbers and lists well-formed, `CORPUS_BASE_MB` and `QUERY_CORPUS_MB` listed in `CORPORA_MB`, and, if weak scaling of build is requested, every size base × N listed too.

## Workload model

Every run has N workers, and each worker performs operations according to the mode: a mode fixes the share of operations that are queries rather than inserts.

| Mode | Query share | Insert share | Starts from | Typical of |
| --- | --- | --- | --- | --- |
| `build` | 0% | 100% | empty index | batch indexing |
| `query` | 100% | 0% | finished index (built first) | serving a fixed index |
| `mixed-95-5` | 95% | 5% | preloaded index | search engine (read-heavy) |
| `mixed-50-50` | 50% | 50% | preloaded index | balanced |
| `mixed-20-80` | 20% | 80% | preloaded index | bulk ingestion with occasional queries |

- **Ratios are per operation, not per thread, and exact.** A run with `--ops K` performs exactly floor(K × insert share / 100) inserts, split exactly among the workers and mixed with the queries in a seeded random order per worker (section Shared implementation rules, Exact operation counts). *Why: works at any N, including 1 and odd values; it is the method of the standard YCSB benchmark. Splitting threads into readers and writers breaks at small N. Exact counts, unlike a random draw per operation, make the final index of a mixed run deterministic.*
- **Mixed modes preload part of the corpus, untimed.** Inserts then take the next documents in order. *Why: queries on an empty index would be artificially fast.*
- **Closed loop:** each worker starts its next operation as soon as the previous one ends. *Why: measures maximum capacity, the right question for comparing strategies. Limitation, stated in the report: closed loop understates tail latency under overload (coordinated omission). An open-loop generator was judged too costly for its benefit.*
- **Strong and weak scaling** in every mode. Strong: fixed total work, more workers. Weak: work proportional to N. *Why: strong scaling shows speedup; weak scaling shows whether larger problems stay manageable.*
  - **`work_n`:** every weak-scaling point is "the work of N". `work_n` records which N's work a run did; it equals `threads` in all runs except a weak-scaling run of a variant whose `max_threads` is below N, which does the work of N (same corpus or `--ops` as the N-worker run) with `max_threads` workers. *Why: this gives T_seq(work of N) for every weak-scaling point, the baseline of absolute speedup in weak scaling.* A run that merges a strong and a weak run (section Results) carries the `work_n` of its weak member.
  - **Build scales the corpus:** strong scaling runs every size in `CORPORA_MB`; weak scaling runs `CORPUS_BASE_MB` × N.
  - **Query and mixed modes run on one fixed corpus** (`QUERY_CORPUS_MB`) and **scale the operations:** `OPS_TOTAL` in strong scaling, `OPS_PER_WORKER` × N in weak scaling; the runner passes the result as `--ops`. *Why: the work of a query run is the number of operations, and a fixed index keeps what is being queried identical across runs; a second corpus-size axis would multiply the sweep for little insight.*
- **Operations beyond the query count:** if a run performs more operations than the query file has lines, each worker starts at an offset in the query file derived from the seed and its worker number, and cycles through the file. *Why: reproducible, different slices of the same query distribution per worker, and any `--ops` works with one query file.*
- **Insert budget:** a mixed run inserts exactly I = floor(`--ops` × insert share / 100) documents, so preload + I must not exceed the number of documents of `QUERY_CORPUS_MB`. The runner checks this for every planned mixed run before starting and stops with an error if it fails. *Why: an overnight run must not die in the middle because the corpus ran out.*

## Data

All implementations read the same preprocessed English Wikipedia, so text handling is decided once and can never differ between languages.

### Corpus

- **Source:** the *English Wikipedia* text corpus of TU Darmstadt (A. Panchenko), Zenodo record [10.5281/zenodo.229904](https://doi.org/10.5281/zenodo.229904), CC BY 4.0: plain text extracted with WikiExtractor from the Wikipedia dump of **26 September 2015**, one file `wikipedia.txt.gz` (4,458,579,201 bytes, MD5 `c1fefd53b73798459e8623e2b965f75f`), downloaded into `data/raw/`. *Why: realistic document lengths, recognizable, and far more text than needed. A fixed, citable snapshot with a checksum makes the corpus reproducible by anyone; extracting a current dump ourselves would depend on the day of the download and on our own markup stripper, which would need its own validation.*
- **Raw format** (inspected on a sample): no `<doc>` tags. An article is a title line, one empty line, then the body with one paragraph or heading per line (headings are plain lines, lists and tables are mostly gone); articles are separated by runs of empty lines. Runs of two or more empty lines also occur *inside* articles (sections whose content was stripped, such as "External links"), so empty lines alone are not a delimiter. Leftover markup is small: HTML-like tags (`<br>`, `</ref>`, `<math>`, `<poem>`), a few `[[File:…]]` lines and URLs.
- **Preprocessing, done once** (`corpus/extract.sh`, `corpus/normalize.sh`, then `corpus/shuffle.sh`): `extract.sh` cuts the raw text into documents and joins each into one line, title first. A **title** is a non-empty line preceded by at least two empty lines (or starting the file), followed by exactly one empty line and then text. *Why: on a 400,000-line sample (4,435 documents) the runs of two or more empty lines number 4,681, of which 246 are inside articles; the title pattern finds 4,435 documents with plausible titles (about 0.6% look odd: a leading space, final punctuation or more than 80 characters).* It also deletes HTML-like tags (`<[^<>]*>`). Then `normalize.sh` lowercases and removes punctuation. Keep stopwords; no stemming or lemmatization. A token is a space-separated string. Finally `shuffle.sh` shuffles the documents once (next item).
- **Shuffle:** after normalization the documents are shuffled once with a fixed seed, `CORPUS_SEED` (2015, in every params file), by `corpus/shuffle.sh`: each line gets a key from a seeded generator of our own (the same Park-Miller generator as the query generator, so the result does not depend on the awk implementation), `sort` orders by key. The prefixes are cut from the shuffled corpus, so each is a random sample of the same corpus and the prefixes are nested. *Why: weak scaling needs statistically similar text at every size. In dump order the document length falls along the file, from about 17 KB per document in the first 256 MB to about 5 KB in the last prefix (long articles come first, stubs later), so a larger prefix would not be the same kind of text, and the number of documents would not grow with the size.* The unshuffled corpus is kept as `data/raw/corpus-dump-order.txt`.
- **Accepted imperfections** (negligible, same for every language): URLs and `[[File:…]]` residue become ordinary tokens (`http`, `www`, `jpg`, `thumb`; below 0.05% of the tokens in the sample); the text of `<math>` stays; a few documents are wrongly split or merged by the boundary rule. *Why: removing these would need a markup parser, which is exactly what the pre-extracted source avoids; the benchmark measures concurrency, not text quality.*
- **Tokenization rule** (`corpus/normalize.sh`): every run of characters that are not letters or digits (any script) becomes one space, so `don't` gives `don t`. Combining marks are not letters, so words in scripts that use them are split (Devanagari `हिन्दी` becomes `हिन दी`); accepted, because it is negligible on English Wikipedia and, since the corpus is built once, identical for every language.
- **Dropped lines:** a line with invalid UTF-8, or with no tokens left after normalization, is dropped, so the document ID is the line number of the *output*. Nothing is dropped silently: `normalize.sh` prints the number of kept and dropped lines to stderr.
- **Format:** one document per line, tokens separated by single spaces, UTF-8. The document ID is the line number, starting at 0. *Why: trivial to parse even in C; implementations make no decisions about text.*

```
the cat sat on the mat
the dog chased the cat
```

### Corpus sizes

- Sizes are **prefixes** of the same corpus (the first 128 MB, 256 MB, …), doubled by **token count**, not bytes or documents. *Why: same kind of text at every size; tokens measure the actual work, while document lengths vary widely.*
- **Measured on the real corpus** (shuffled, `make-prefixes.sh bench/params.sh`; 4,508,255 documents and 11.35 GB in total after `extract.sh` and `normalize.sh`, 43 lines without tokens dropped, none with invalid UTF-8):

| `CORPORA_MB` | tokens | bytes | documents |
|---|---|---|---|
| 128 | 21,586,249 | 128,001,297 | 50,930 |
| 256 | 43,173,888 | 255,794,024 | 101,062 |
| 512 | 86,345,215 | 511,381,951 | 203,212 |
| 1024 | 172,690,058 | 1,022,842,023 | 406,913 |
| 2048 | 345,380,722 | 2,045,486,778 | 813,625 |

  The base is **128 MB** (`CORPUS_BASE_MB=128`): its token count T = 21,586,249 is the first whole documents that reach 128 MB, and every other size is the first whole documents that reach T × size / 128 tokens. The largest size is 2048 MB, which is also the weak-scaling size at 16 workers (128 × 16). *Why 128 and not 256 MB: with a 256 MB base, weak scaling at 16 workers needs 4096 MB, where Java (about 18 GB, extrapolated) and Go (14.3 GB, measured) do not fit the 20 GB budget with any margin once concurrent variants add their own memory (section Memory budget, Sizing measurements).* An earlier cut with base 256 MB (sizes 256 to 4096 MB, e.g. 101,131 documents at 256 MB and 1,624,536 documents at 4096 MB) was used for the sizing measurements; the sizes 256 to 2048 MB were cut again from the same shuffled corpus, and differ from the earlier ones by less than 0.2% in size.
  The number of documents is now proportional to the size (about 2.5 KB and 430 tokens per document at every size). Preprocessing the whole dump takes about 16 minutes (`extract.sh` + `normalize.sh`), shuffling 1 minute, cutting the prefixes 10 s, generating 100,000 queries 51 s. Before the shuffle (dump order) the 256 MB prefix had only 15,005 documents.
- Vocabulary grows slower than text (Heaps' law): twice the text gives fewer than twice the distinct terms. Accepted and noted in the report.
- Weak-scaling sizes of build mode are multiples of a base unit (128 MB at 1 worker, up to 2048 MB at 16). Every corpus size the runner uses (`CORPORA_MB`, base × N, `QUERY_CORPUS_MB`) must be a prefix listed in `CORPORA_MB`; `make-prefixes.sh` cuts exactly those.

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
- **Generation:** terms are drawn from the preloaded part of the corpus (the first `PRELOAD_DOCS` = 100,000 documents; every prefix with at least that many documents gives the same file, `corpus/make-queries.sh` is run on the `QUERY_CORPUS_MB` prefix) by picking random word occurrences; the operator is chosen at random; fixed seed. *Why: queries hit real documents; frequent words appear often, as in real searches (picking from the vocabulary would give mostly rare words); every variant gets identical queries.*

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
- **Every repetition starts from scratch with the same seeds:** a fresh empty index, then the build (query mode) or the preload (mixed modes), then the timed phase with the generators re-initialised from `--seed`. *Why: all repetitions do identical work and differ only in timing; otherwise the second mixed repetition would start from an index that already holds the inserts of the first.*

## Shared implementation rules

Every implementation follows the same rules for randomness, operation counts, queries, settings and data structures, so that any difference in the results comes from the concurrency strategy and the language, not from different work.

### Implementation order

- Default language order: **Java, Go, C++, C**.
- Java `sequential` is implemented first, completely (all five modes, latencies, dump, measurements), together with `impl/java/_common/`. *Why: it is the first end-to-end test of the infrastructure; problems in the contract are cheap to fix while only one implementation exists.*
- Go, C++ and C `sequential` follow, using the Java version as the reference. `bench/check.sh` must pass for all four.

### Pseudo-random generator (identical in every language)

Every implementation uses **splitmix64**, written in its own `_common/`. No language's built-in generator is used. *Why: "fixed seed" gives the same operations everywhere only if every language computes the same sequence.*

```
state = state + 0x9E3779B97F4A7C15          (64-bit, wrapping)
z = state
z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9    (64-bit, wrapping)
z = (z ^ (z >> 27)) * 0x94D049BB133111EB    (64-bit, wrapping)
return z ^ (z >> 31)
```

- All values are **unsigned 64-bit**; shifts are logical. In Java: `>>>` for shifts, `Long.remainderUnsigned` for modulo.
- A random index below `m` is `next() mod m` (unsigned). The tiny modulo bias is accepted: it is identical in every language.
- Worker `i` (0-based) initialises its generator with `state = seed + i`.
- **Order of draws per worker** (fixed, so all languages match):
  1. one draw for the starting offset in the query file: `offset = next() mod Q` (Q = number of queries);
  2. the draws of the operation shuffle (below), in order.

### Exact operation counts

A run has exactly `--ops K` operations, split exactly; ratios are exact, not probabilities.

- Insert share `s` per mode: query 0%, mixed-95-5 5%, mixed-50-50 50%, mixed-20-80 80%.
- Total inserts: `I = floor(K * s / 100)`. Total queries: `K - I`.
- Split among N workers (0-based `i`):
  - operations: `ops_i = floor(K / N) + (i < K mod N ? 1 : 0)`
  - inserts: `ins_i = floor(I / N) + (i < I mod N ? 1 : 0)`
  - queries: `ops_i - ins_i`
- Each worker builds an array of its operation types (`ins_i` inserts, then the queries) and shuffles it with **Fisher–Yates** using its own generator: for `j` from `ops_i - 1` down to `1`: `k = next() mod (j + 1)`; swap `j` and `k`. The array is built **before** timing starts.
- Inserted documents are taken **in corpus order** after the preloaded part, through a shared counter: whichever worker inserts next takes the next document.
- Consequence: the final index of a mixed run is always *preload + the next I documents*, independent of timing. Mixed runs are deterministic and can be checked by `bench/check.sh` (section Correctness), and the sizing check knows exactly how many documents a run needs (`preload + I`, section Workload model).
- Build mode is unchanged: the whole corpus, no `--ops`.

### Conventions of the reference implementation (Java)

Details that the rules above leave open; fixed by the Java implementation (`impl/java/_common/`) and followed by Go, C++ and C. All Java sources are in the default package: `build.sh` compiles `_common/*.java` and the variant's own files together into `build/`.

- **Repetition numbers** start at 0 and count warm-ups first (`rep` 0 … W+R−1); `warmup` is 1 for the first W.
- **`load_ns`:** every repetition reads the whole input file again into memory, one string per line (document ID = position). Splitting a line into tokens is part of indexing (`build_ns`, `preload_ns`, the timed inserts), not of loading. The previous repetition's data is unreachable by then, so repetitions do not accumulate memory.
- **Phases:** `build` mode: `build_ns` is indexing the whole corpus, then `finalize_ns` is `finish()`. `query`: `build_ns`, then `finalize_ns` (the queries need the finished index), then `run_ns`. Mixed: `preload_ns`, `run_ns`, then `finalize_ns` after the run. `ops_query` and `ops_insert` are 0 in build mode. `finalize_ns` is measured even when `finish()` does nothing, as in the sequential variants, whose posting lists are sorted by construction.
- **Query cursor:** a worker's k-th query operation (k from 0) is query number `(offset + k) mod Q`; inserts do not advance it.
- **Latency:** each query is timed with two clock readings around it, inside `run_ns` (the cost of the readings is part of the measurement, the same in every language). At startup every implementation measures the cost of one clock reading (1,000,000 back-to-back readings, mean in ns, integer division) and prints it to stderr as `clock reading cost: <n> ns (mean of 1000000 back-to-back readings)`, so every run's log has it. **Query latencies include this overhead**: a latency is the difference of two readings, so it contains roughly one reading's cost on top of the query (a few tens of ns; the analysis can subtract the logged value, or report it next to the percentiles). Java's number includes the interpreter and JIT warm-up of the measuring loop itself. Latencies are written to `--latencies` after the repetition's row measurements are taken; the file is opened once and holds all repetitions.
- **Dump:** written once, after the last repetition (from that repetition's index). Terms are sorted by their **UTF-8 bytes** (the order of `LC_ALL=C sort`), not by the language's native string order.
- **Argument checking:** options that do not belong to the mode (`--queries`, `--ops`, `--seed`, `--latencies` outside query and mixed; `--preload` outside mixed) are errors, like missing required ones and numeric parameters not in the variant's `uses`; a `--threads` above the variant's maximum is an error.
- **Process measurements:** see Process measurements below; Java calls `getrusage` through the FFM API (`java.lang.foreign`, `impl/java/_common/Proc.java`), which is why the JVM runs with `--enable-native-access=ALL-UNNAMED`.
- **Sequential variant:** `partitioning` `single`, `writer` `direct`, `write_timing` `immediate`, no `coordination`, `granularity` or `visibility`, `uses` `none`, `max_threads` 1. The index is one `HashMap`; a term seen earlier in the same document is the last posting of its list, so the frequency is counted without a per-document map. `build.sh` writes `javac` and `java` versions and the run flags (`-Xmx20g`, `--enable-native-access=ALL-UNNAMED`) to `build/settings.txt`.

**Go** follows the Java conventions above (same phases, columns, argument checks, dump order, query cursor and latency timing); `getrusage` is `syscall.Getrusage`. What is specific to Go:

- **Build:** `go build` accepts named `.go` files from one directory only (`named files must all be in one directory`), so compiling `_common/` and a variant together cannot be a single command on the original folders. `build.sh` therefore copies `_common/*.go` and the variant's `*.go` into `build/src/` and runs `go build` there on `./*.go`, as package main, with no `go.mod`; the binary is `build/index`, and `run.sh` executes it. A file name that exists in both `_common/` and the variant is an error (`build.sh` stops and names the file): the copy would silently overwrite one of them. *Why: keeps `_common/` shared as in Java, with no module and no tool beyond `go`; the cost is that compiler messages point to the copies in `build/src/`, which have the same names as the originals.* All Go sources are `package main`.
- **Index interface:** `Each(visit)` replaces Java's `entries()`; posting lists are two `[]int32` slices grown with `append` (Go's own growth policy, not Java's doubling from 2). Tokens are substrings of the document line (no copy), but a term is cloned (`strings.Clone`) when it is inserted as a new map key, so the index owns its keys, as Java's does, and never keeps a document line alive. *Why: same memory semantics in both languages; without the clone a key would pin its whole line.*
- **Sequential variant:** one built-in `map[string]*PostingList`; `build.sh` writes `go version` to `build/settings.txt`; `GOMAXPROCS` is logged on stderr at start.
- **Cross-check against Java** (8 MB corpus, 1 worker, 1 warm-up + 1 repetition): identical dumps in all five modes, and identical sums of query result sizes on stderr in query mode and, since one worker makes them deterministic, in the mixed modes.

**C++** follows the Java conventions above as well; `getrusage` is called directly, time is `std::chrono::steady_clock`. What is specific to C++:

- **Build:** `g++ -O2 -std=c++17 -I _common _common/*.cpp <variant>/*.cpp`, one command, output `build/index`; `build/settings.txt` records the `g++` version and flags. Shared code is a set of headers and sources in `_common/`, the variant includes them through `-I`.
- **Structure:** `Index` is an abstract class (virtual `add`, `get`, `finish`, `each`, …), as Java's and Go's are interfaces; the runner is a normal function, not a template. A virtual call costs a few ns per document and per query term, the same kind of cost the other two languages pay.
- **Cost of the standard map, per token:** the index is `std::unordered_map<std::string, PostingList>`, and C++17 has no lookup by `std::string_view` in it (heterogeneous lookup arrived in C++20). The plain standard approach is used: each token (a `string_view` into the document line) is copied into a temporary `std::string`, which is then given to `try_emplace` (one hash, one lookup, the string is moved into the new node only if the term is new, otherwise it is destroyed). So **every token costs one string construction and destruction** (copying its bytes), and a **heap allocation only if the token is longer than 15 bytes**, the small-string limit of libstdc++ (0.07% of the tokens of the 8 MB corpus; 959 of 1,343,362). Queries do not pay this: their terms are already `std::string`s. *Why: no workaround such as a custom hash map or a `string_view`-keyed map, which would change what "the standard map" measures; the cost is part of what the benchmark compares, and the report says so.*
- **Posting lists** are two `std::vector<int32_t>` (capacity doubles), stored by value in the map nodes; the dump sorts `std::string`s, which compare as unsigned bytes.
- **Cross-check against Java** (8 MB corpus, 1 worker, 1 warm-up + 1 repetition; `--ops 20000`, `--ops 3000` for the two mixed modes with the larger insert shares): identical dumps in all five modes and identical sums of query result sizes.

**C** follows the Java conventions as well; `getrusage` is called directly, time is `clock_gettime(CLOCK_MONOTONIC)`. What is specific to C:

- **Build:** `gcc -O2 -std=c17 -I _common -I <variant> _common/*.c <variant>/*.c`, one command, output `build/index`; `build/settings.txt` records the `gcc` version and flags. Sources that need POSIX functions (`clock_gettime`, `getrusage`, `getline`) define `_POSIX_C_SOURCE 200809L` themselves, so the compiler flags stay exactly `-O2 -std=c17`. Errors end the process through `fatal()` (message on stderr, exit status 1).
- **Structure:** `Index` is a struct of function pointers (the counterpart of the abstract class in C++ and the interface in Java and Go); a variant embeds it as the first member of its own struct. The documents of a repetition are read into one buffer and are views into it; tokens are `(pointer, length)` pairs, so the C index copies a term's bytes only once, when the term is new.
- **Hash map (our own choice):** C has no standard hash map, so `_common/hashmap.c` provides one. Design: open addressing with linear probing, no deletion, **FNV-1a 64-bit**, capacity a power of two (initially 1024) that **doubles when the load factor would exceed 0.5** (the table is then rebuilt from the stored hashes). A slot is `(hash, owned key pointer, key length, pointer to a separately allocated posting list)`; the hash is compared before the key bytes (`memcmp`). Posting lists double like Java's (capacity 2, then ×2). *Why: open addressing with linear probing is the simplest design that is cache-friendly and short enough to explain line by line; no deletion because the index never removes terms; FNV-1a because it is a few lines and needs no tables; load factor 0.5 keeps probe sequences short, at the price of memory. The list lives outside the slot so that **posting lists never move when the table grows**: a pointer to a list stays valid, which the concurrent variants need to give each list its own lock later.* **Because the map is our own while Java, Go and C++ use their standard maps, part of C's result comes from the map, not only from the language.** The report says so. Unlike C++ (one string copy per token) and Go (map lookup on a string), a lookup here takes no copy; this is a property of the design, not an optimisation applied to C only.
- **Cross-check against Java** (8 MB corpus, 1 worker, 1 warm-up + 1 repetition; `--ops 20000`, `--ops 3000` for the two mixed modes with the larger insert shares; `--preload 500`): identical dumps in all five modes and identical sums of query result sizes.

### Concurrent variants (round 1: `global-lock`, `partition-lock`)

The questions and the variants are in `docs/experiment-plan.md`; this section fixes what that plan leaves to the implementation. It applies to every language.

- **One worker loop for every variant, `sequential` included.** Every phase (build, preload, the timed query or mixed phase) runs on `--threads` worker threads, also with one (`Workers` in `_common/`). Documents are handed out by a shared atomic counter (`d = counter++`, once per document): build takes documents `0 … D-1`, preload `0 … preload-1`, and the inserts of a mixed run continue from the same counter at `preload`. Each worker follows its own operation plan (exact counts, section Exact operation counts); `ops_query` and `ops_insert` are the totals over all workers. Worker handles (private buffers) are made before timing starts; thread creation and joining are inside the timed phase. The `sequential` rows of `results/full/20261115-162825` were measured before this change (one thread, no counter); the added cost is one atomic increment per document and one thread start per phase, negligible against about 2 ms per document. *Why: one harness for all variants, so that differences come from the strategy.*
- **Index and worker handle.** `Index` makes one `Worker` per thread (`add(docId, doc)` and `query(...)`); everything a worker needs privately (tokenizing buffer, grouping arrays, query scratch space) lives in its handle. `get(term)` has no synchronisation of its own: a query calls it with the variant's locks held. `docs()` is counted with a contention-free counter (`LongAdder` in Java), `postings()` under the lock that protects the lists.
- **Per-document postings.** A concurrent worker first turns its document into its distinct (term, frequency) pairs, in a private hash map (`DocPostings`; the sequential variant avoids this map by looking at the last posting of the list, which a shared list cannot allow). That is an extra cost of the concurrent variants, paid also at one worker. **The sequential variant keeps its counting shortcut** (it is the best sequential baseline), so a variant's overhead at one worker, in the analysis ("Concurrency overhead"), includes this per-document counting and the buffering, not only synchronization.
- **`batch` counts postings and crosses documents.** A posting is one (term, document, frequency) triple. Each worker keeps one private buffer of postings (`PostingBuffer`): one in `global-lock`, one per partition in `partition-lock`. A document's postings are appended to the buffers, and a buffer is flushed, under its lock, as soon as it holds exactly `batch` postings, whatever documents they come from. At the end of every phase (the indexing loop of build, query-mode build and preload, and the worker's part of the timed query or mixed phase) each worker flushes all its non-empty buffers, inside the timed phase, so nothing stays buffered when a phase ends. With `batch` 1 every posting is flushed at once (one acquisition per posting). *Why: with a buffer that never crosses a document, `batch` 1024 could never fill (a document has about 180 distinct terms: 149,009,737 postings in 813,625 documents) and `partition-lock` would take about P locks per document whatever `batch` is.* Consequence: a document's postings can reach the index later than the document's `add`; in mixed modes a query may miss postings still in a worker's buffer (partial visibility, as intended), and the final index is complete after the flush. The worker handle has a `flush` operation for this; the sequential handle's is empty.
- **`partition-lock` order.** Worker number `w` (0-based) visits the partitions in the order `w mod P`, `w mod P + 1`, …, wrapping around to 0, both when it appends a document's postings to its buffers (and so when a full buffer is flushed) and when it flushes everything at the end of a phase; one lock at a time. *Why: if every worker went 0 … P-1, they would all queue at partition 0 first (a convoy). Queries keep the fixed order lower partition number first, with two locks held at once.* Worker handles are made with their number, `index.worker(number)`.
- **Queries hold the locks for the whole evaluation:** `global-lock` takes the one lock; `partition-lock` the locks of the partitions of both terms, lower number first, once if equal (plan: Fixed lock order). The partition of a query term is computed by the query and again by `get`.
- **Sorted insertion** (`PostingList.insert`): walk back from the end while the stored ID is larger, then insert there; almost always a plain append.
- **Partition hash** (`PartitionHash`): FNV-1a 64-bit over the UTF-8 bytes, `(hash >> 32) mod P`; the same in every language.
- **Dimensions** (`dimensions.tsv`): `global-lock` is `single` / `direct` / `lock` / granularity `index` / `immediate` / visibility `partial`, `uses batch`; `partition-lock` is `by-term` / `direct` / `lock` / granularity `partition` / `immediate` / `partial`, `uses partitions,batch`; both support all five modes and have no `max_threads`.

**Java.** The buffer is `PostingBuffer` (arrays of terms, document IDs and frequencies; the term strings are the ones `DocPostings` made). `Workers.run` starts one platform `Thread` per worker and joins them; a failure in a worker is rethrown in the main thread. Locks are `ReentrantLock` (default, non-fair). `PartitionHash` encodes UTF-8 by hand, code point by code point, to avoid a byte array per term.

**Go.** The buffer is `PostingBuffer` (slices; terms are substrings of the document lines, cloned by the index when new). `RunWorkers` starts one goroutine per worker and waits with a `sync.WaitGroup` (a panic in a goroutine ends the process with a non-zero status); the counter is `atomic.Int64`, `docs` an `atomic.Int64` too. Locks are `sync.Mutex`. `DocPostings` counts in a `map[string]int32` that is emptied (`clear`) for every document; its terms are substrings of the document line, and the index clones a term (`strings.Clone`) when it becomes a new key, as in the sequential variant. `PartitionOf` hashes the bytes of the Go string directly (already UTF-8). Both variants declare no `max_threads`; `main.go` passes `math.MaxInt32`. **Cross-check against Java** (8 MB corpus, 8 workers, 1 warm-up + 1 repetition, `--ops 3000`, `--preload 500`): identical dumps in all five modes against the sequential dump, and the same sum of query result sizes as Java at 8 workers (2,335,481; the sum depends on the worker count, because each worker starts at its own offset in the query file).

**C++.** The buffer (`PostingBuffer`) owns a `std::string` copy of every term, because `DocPostings` points into a map that is emptied for the next document; the flush moves the strings into the index map (`try_emplace` with an rvalue key), so a new term costs no further copy. `runWorkers` starts one `std::thread` per worker and joins them; an exception in a worker is caught there and rethrown in the main thread. `Index::worker()` returns a raw `Worker*` owned by the index (the sequential index is its own handle). `build.sh` keeps the flags `-O2 -std=c++17` unchanged: with this glibc `std::thread` and `std::mutex` need no `-pthread`. Locks are `std::mutex` taken through `std::lock_guard` / `std::unique_lock`; `docs` is a `std::atomic<int>`. `DocPostings` counts in an `unordered_map<std::string, int>` (one `std::string` per token, as in the sequential convention above) and exposes pointers to its keys; the index copies a term only when it is new (`try_emplace`). `PostingList::insert` uses `std::vector::insert`. **Cross-check against Java** (same setup as Go): identical dumps in all five modes, and the 8-worker query sum 2,335,481.

**C.** The buffer (`PostingBuffer`) holds `(pointer, length)` views into the document lines, which stay in memory until the repetition ends; nothing is copied. `workers_run` starts one `pthread` per worker and joins them (`fatal()` in a worker ends the process). Locks are `pthread_mutex_t` with default attributes; `docs` is an `atomic_int`. `build.sh` keeps the flags `-O2 -std=c17`: with this glibc `pthread_*` needs no `-pthread`. `index->worker()` returns a handle owned by the index and freed in `destroy`; the sequential index has a single handle. `fnv1a` moved from `hashmap.c` to `_common/partitionhash.h`, so the map and the partition hash use the same function (the map takes the low bits, the partition the high ones). `DocPostings` is a small open-addressing table (linear probing, load factor at most 0.5, cleared after each document by visiting the terms it holds) of `(pointer, length, hash, frequency)` entries, with no copy of the text; the partition of a posting is computed from the stored hash, so a term is hashed once per document in the partition-lock insert path (the map hashes it again when it inserts). **Cross-check against Java** (same setup as Go): identical dumps in all five modes, the 8-worker query sum 2,335,481, and no ThreadSanitizer report for either variant in `mixed-50-50` with 8 workers (4 MB corpus; run with `setarch -R`, since TSan needs ASLR off).

### Queries

- Each worker starts at its `offset` in the query file and **cycles** through it.
- A query result is the set of document IDs (frequencies are not used).
- Algorithms, identical in every language:
  - `SIMPLE t`: the posting list of `t` (empty if the term is absent).
  - `AND a b`: intersection of the two sorted lists with a **two-pointer merge**.
  - `OR a b`: union of the two sorted lists with a **two-pointer merge**, without duplicates.
- Only query operations record a latency; inserts do not.
- **Results are consumed:** each worker adds up the sizes of its query results; after timing, the total over all workers is written to stderr, one line per repetition. *Why: a result that nothing reads may be removed by the JIT (Java) or the optimizer (`-O2`), and the query timings would then mean nothing. stdout stays unchanged. In query mode the total must be identical across variants and languages (same index, same queries), which is a free cross-check in the logs; in mixed modes it depends on timing.*

### Process measurements (identical in every language)

**Every language takes all process measurements from `getrusage(RUSAGE_SELF)`**, called around each repetition (Java through the FFM API, Go with `syscall.Getrusage`, C and C++ directly): `peak_rss_bytes` is `ru_maxrss` (kilobytes on Linux, times 1024), `cpu_user_ns` and `cpu_sys_ns` are the differences of `ru_utime` and `ru_stime` (microsecond resolution), `ctx_vol` and `ctx_invol` the differences of `ru_nvcsw` and `ru_nivcsw`. *Why: one system call for all columns, the same in every language and with the same units, covering all threads of the process (also those that have ended), with no parsing of text files and no dependence on the runtime's own accounting.*

### Build and runtime settings (as standard as possible)

| Language | Build | Run |
| --- | --- | --- |
| Java | plain `javac` (JDK 25), no Maven/Gradle | `java`, default GC, fixed `-Xmx20g`, `--enable-native-access=ALL-UNNAMED` |
| Go | plain `go build`, defaults, no `go.mod` (section Conventions of the reference implementation, Go) | defaults (`GOMAXPROCS` not changed, but recorded) |
| C++ | `g++ -O2 -std=c++17` | — |
| C | `gcc -O2 -std=c17` | — |

- `-Xmx20g` and `--enable-native-access=ALL-UNNAMED` are the only non-default settings. *Why: Java's default maximum heap is ¼ of RAM (~8 GB), which the largest corpus can exceed; 20 GB matches the memory budget. The second flag allows the native call to `getrusage` (Process measurements) and silences the JVM's warning about it, which would otherwise appear on stderr of every run.*
- Go records its `GOMAXPROCS` (`runtime.GOMAXPROCS(0)`) on stderr at start, so it is in the run's log. *Why: the default follows the CPU affinity, so it is 22 unpinned and 12 under `taskset` to the P-cores; the log shows which value each run had.*
- `build.sh` records the compiler/JVM version and flags in `build/settings.txt` (column `build_settings`, section Results).

### Data structures

- The index is a hash map from term to posting list. Each language uses **its standard hash map**: `HashMap` (Java), built-in `map` (Go), `std::unordered_map` (C++). *Why: the most natural baseline per language; differences between standard maps are part of what the benchmark compares, and the report says so.*
- C has no standard hash map: its `_common/` has a hand-written one (open addressing, linear probing, FNV-1a; section Conventions of the reference implementation, C). It is our own choice, so part of C's result comes from the map.
- Posting lists are growable arrays of (doc ID, frequency).

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
- `work_n` is the last column of a row (new columns are added at the end): the N whose work the run did, see Workload model.
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

`failures.tsv` has the columns `run`, `phase`, `reason`, `status`, then `language`, `variant` and the run's parameters (all of them except `seed`, and `work_n` last) with the same names and values as in `results.tsv`. `phase` is `discover` (variant skipped), `build` or `run`. `reason` is a short text for `discover` (for example `run.sh missing or not executable`), `build-failed` for `build`, and for runs `exit`, `timeout`, `bad-output` (wrong row or field count) or `bad-latencies`. `status` is the exit code (124 or 137 for a timeout), `-` if there is none. `run` and the parameters are `-` where they do not apply (discover, build). The stdout of a successful run is deleted after its rows have been appended to `results.tsv`; that of a failed run is kept for diagnosis.

`meta.txt` records the preflight state (power source, swap in use, swap pages moved, platform profile, CPU governor and energy preference) before the sweep and again after it, the `SHUFFLE_SEED`, the physical and logical CPU counts, and a copy of the params file.

## Analysis plan

Python reads the final `results.tsv` and the latency files and computes the standard concurrent-performance metrics; nothing below is computed by the implementations. T(p) is the time with p workers.

| Metric | How it is computed | What it reveals |
| --- | --- | --- |
| Execution time | `build_ns`, `run_ns`, phases | the raw basis of everything else |
| Relative speedup | T(1) / T(p), same variant | how well a variant scales |
| Absolute speedup | T(sequential) / T(p); in weak scaling T_seq of the same work, from the sequential rows with the same `work_n` | whether parallelism actually paid off |
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

With `--dump`, a variant writes its final index in one fixed text form, in a separate untimed run: terms sorted by their UTF-8 bytes, one per line, postings `docid:frequency` sorted by document ID.

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
- It works for `build` and `query` modes, and for the **final index** of mixed modes: inserts are exact counts taken in corpus order, so the final index of a mixed run is always the first preload + I documents, whatever the timing (section Shared implementation rules, Exact operation counts).
- It is the only check that would catch a variant that is fast because it loses updates, the typical race-condition bug. Worth running at least once per variant before trusting its numbers.
- **`bench/check.sh [params-file]`** automates it. It discovers the variants like the runner, builds each, and runs it with `--dump` on the hand-made test data (`data/tiny/`: corpus, queries and `expected-dump.tsv`) in every mode the variant declares, once for every worker count in `THREADS` of the params file (default `params-quick.sh`) up to the variant's `max_threads`, `CHECK_REPEAT` repetitions and `CHECK_WARMUP` warm-ups each; numeric parameters in `uses` take the values `CHECK_PARTITIONS`, `CHECK_BATCH` and `CHECK_QUEUE` of the params file, and query and mixed modes get `--ops CHECK_OPS`, mixed modes `--preload CHECK_PRELOAD` (all in the params file, like every numeric parameter). The dump is compared with `cmp`:
  - `build` and `query`: against the hand-made `data/tiny/expected-dump.tsv`.
  - mixed modes: against a **reference dump** of the same variant. `check.sh` cuts the first preload + I lines of the tiny corpus (`head -n`) into a temporary file, runs the variant in `build` mode with one worker on it, and compares the two dumps. *Why: the build dump is itself checked against the hand-made one, so the mixed check catches lost or duplicated inserts without a hand-made dump per mode and per `CHECK_OPS`.* A variant that declares mixed modes but not `build` fails the mixed check (detail: no build mode for the reference).
  - Before running, `check.sh` checks that `CHECK_PRELOAD` + I of the largest insert share fits in the documents of the tiny corpus, and stops with an error if not.

  stdout is a TSV with one row per variant (`variant`, `result` = `PASS` or `FAIL`, `detail`), stderr shows progress, the exit code is 0 only if every variant passed. *Why: worker counts above the number of documents and above the number of cores are where lost updates and edge cases show up, and the test corpus is small enough to run them all in seconds.*
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
6. Build the list of runs: variant × mode (from `modes`) × corpus × threads × the parameters in `uses` × pinning × scaling; thread values above `max_threads` are skipped in strong scaling, and become capped runs with `work_n` = N in weak scaling. Build scales the corpus, query and mixed use `QUERY_CORPUS_MB`. Compute `--ops` for each. Merge runs with identical arguments, which differ only in scaling (and `work_n`), into one run with `scaling` `strong,weak`. Check that every data file exists and that the mixed runs fit in the corpus (preload + I documents, section Workload model, Insert budget); stop with an error if not.
7. **Shuffle the list**, with a pseudo-random order that depends only on `SHUFFLE_SEED` (a small seeded generator inside `run.sh`; the same params file gives the same order, and the seed is in `meta.txt`). *Why: a laptop heats up over a session; random order keeps throttling from always hitting the same variant. A fixed seed keeps the sweep reproducible.*
8. For each run: optional cool-down pause, then `run.sh` under `timeout`, prefixed with `taskset` when pinned. Check the output, prepend context columns to each stdout row and append to `results.tsv`; save stderr to `logs/`.
9. A non-zero exit, a timeout, malformed output or a malformed latency file is recorded in `failures.tsv` as a failed run, and the runner continues. When pinned to P-cores, runs with more workers than P-core threads are skipped.
10. After the sweep, record the preflight state again in `meta.txt`; warn (do not stop) on battery power or if swap pages were moved during the sweep.

**Rules**

- **Timeouts are generous:** 30 minutes per run to start, tightened later to several times the slowest normal run. *Why: a deadlock would otherwise hang an overnight run forever; standard `timeout` adds no dependency.*
- **Build and runtime settings are fixed and recorded:** compiler flags in `build.sh`, JVM heap (`-Xmx`) in `run.sh`, `GOMAXPROCS` left at its default and logged (section Shared implementation rules, Build and runtime settings). *Why: fairness depends on how code is compiled and run, not just on the code.*
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

Adding a worker does not add a fixed amount of compute: speedup curves bend where a core type runs out. The CPU numbers of each type (`/sys/devices/cpu_core/cpus`, `cpu_atom/cpus`): P-cores with hyperthreading are CPUs 0-11, E-cores and LP E-cores are CPUs 12-21.

### Thread values

- Strong scaling: `1 2 4 6 8 12 14 16 22`. *Why: includes the hardware boundaries, which are marked on every plot.*
- Weak scaling: `1 2 4 8 16`. *Why: doubling, to match the corpus doubling.*

### Pinning

- **Main experiment unpinned** (`params.sh`, `PINNING=none`): the OS schedules freely, as real software runs.
- **Control experiment pinned to P-cores** (`taskset`), as a separate, smaller experiment: `bench/params-pinned.sh`, with its own `EXPERIMENT` name (`pinned`) and results folder. It runs strong scaling only, workers up to 12 (the P-core threads), the modes build, query and mixed-50-50, two corpus sizes for build, and one value for each numeric parameter; every value is also in `params.sh`, so rows compare directly. *Why: if the bend at 6 workers disappears when pinned, it is proven to be the hardware, not contention. Turns a guess into evidence, without doubling the main sweep.*

### Memory budget

- About **20 GB** for the benchmark; **swap must never be used.** *Why: swapping would dominate the measurements.*
- **Measured** (section Sizing measurements): the peak of a build run is the input text (the whole file is read into memory, 1× its size) plus the index. The index alone is about **1.0× the text for C and C++, 2.5× for Go and 3.5× for Java** (2048 MB corpus; Java is 5.8× on the smallest corpus because of the JVM's fixed overhead). Per posting, peak RSS is about 28 bytes for C and C++, 48 for Go and 61 for Java (2048 MB). The first estimate (1.5× compact, up to 5×) was too high for C/C++ and about right for Java.
- Largest corpus prefix = **2 GB** of text (2048 MB); weak-scaling base = **128 MB** (≈ 21.6 million tokens). Measured peaks of the sequential variants at 2048 MB: Java 9.1 GB, Go 7.1 GB, C++ 4.0 GB, C 4.2 GB. Concurrent variants need more (per-worker buffers, locks, queues), so 2048 MB leaves a margin of about 2× below the 20 GB budget for the worst language. At 4096 MB Java (about 18 GB extrapolated) and Go (14.3 GB) would not fit, which is why that size was dropped (section Corpus sizes).
- Mixed modes need enough documents after the preloaded part for all inserts; sized together with `--ops` and checked by the runner before the sweep (section Workload model).

### Sizing measurements (sequential baselines)

Raw data: `results/sizing/build.tsv` and `results/sizing/query.tsv` (the measurement columns of section Results, plus `language`, `corpus_mb` or `ops`, and `wall_s`; one row per repetition, warm-up included). Measured on 2026-11-15 on the benchmark machine, unpinned, one language at a time, with the four `sequential` variants, **no implementation changed**: `--threads 1 --repeat 1 --warmup 1`, 5 s pause between runs, and a guard that skipped the next corpus size of a language if twice the last peak would not fit in the free memory. Tables show the measured repetition (`warmup` 0); `peak_rss_bytes` is process-wide, so it includes the warm-up repetition (section Memory semantics).

**Build mode, every corpus** (`--mode build`). B/posting is `peak_rss_bytes / postings`; "index only" subtracts the input text (corpus bytes, section Corpus sizes) first. Postings are identical in all four languages (a cross-check), as are documents and terms.

| language | corpus MB | build_ns (s) | peak (GB) | postings | B/posting | B/posting, index only | note |
|---|---|---|---|---|---|---|---|
| java | 256 | 3.7 | 1.75 | 18575496 | 94.1 | 80.3 | |
| java | 512 | 7.6 | 2.93 | 37268327 | 78.5 | 64.8 | |
| java | 1024 | 16.4 | 5.12 | 74543434 | 68.7 | 54.9 | |
| java | 2048 | 35.7 | 9.11 | 149009737 | 61.2 | 47.4 | |
| go | 256 | 2.5 | 0.82 | 18575496 | 44.4 | 30.6 | |
| go | 512 | 5.4 | 1.61 | 37268327 | 43.1 | 29.4 | |
| go | 1024 | 11.8 | 3.73 | 74543434 | 50.0 | 36.3 | |
| go | 2048 | 23.5 | 7.07 | 149009737 | 47.5 | 33.7 | |
| go | 4096 | 50.8 | 14.31 | 297782777 | 48.1 | 34.3 | † |
| cpp | 256 | 3.8 | 0.61 | 18575496 | 32.6 | 18.8 | |
| cpp | 512 | 7.8 | 1.14 | 37268327 | 30.7 | 17.0 | |
| cpp | 1024 | 16.7 | 2.10 | 74543434 | 28.1 | 14.4 | |
| cpp | 2048 | 35.2 | 4.03 | 149009737 | 27.0 | 13.3 | |
| cpp | 4096 | 72.8 | 8.14 | 297782777 | 27.3 | 13.6 | |
| c | 256 | 2.2 | 0.62 | 18575496 | 33.2 | 19.4 | |
| c | 512 | 4.6 | 1.22 | 37268327 | 32.7 | 18.9 | |
| c | 1024 | 9.5 | 2.13 | 74543434 | 28.6 | 14.9 | |
| c | 2048 | 19.3 | 4.22 | 149009737 | 28.3 | 14.6 | |
| c | 4096 | 41.2 | 8.32 | 297782777 | 27.9 | 14.2 | |

† **Not valid.** The machine had about 15.8 GB of memory available (a browser and other programs were open, 31 GB installed) and the run peaked at 14.3 GB: about 2 GB of other programs were pushed to swap (swap went from 0 to full), and the run itself shows it (system CPU time 2.4 s against 0.6 s at 2048 MB, voluntary context switches 20,769 against 4,355). The run must be repeated with the other programs closed before it is used. It also breaks the rule that swap is never used; the guard of the sweep was too optimistic (it only required the prediction to fit in the available memory, with no margin). All other runs stayed in RAM (a few MB of swap was touched once, before Go 4096).
- **Java 4096 MB was not run:** the guard predicted about 19 GB (Java's peak grew ×1.67, ×1.75 and ×1.78 per doubling), above the memory available and too close to `-Xmx20g`.
- Build time per corpus doubling is about ×2.0–2.2 in every language: linear in the text.

**Query mode, 256 MB corpus** (`--mode query`, `--queries data/queries.txt`, 100,000 queries, `--seed 42`, 1 warm-up + 1 repetition, so the latency file holds two repetitions). `run_ns` is the measured repetition. The sum of query result sizes is the same in all four languages (26,207,950,630 for 1,000,000 operations, both repetitions), and the clock reading cost logged at startup was 12–27 ns.

| language | `--ops` | run_ns (s) | mean µs per query | latency file (bytes) | bytes per latency line |
|---|---|---|---|---|---|
| java | 10000 | 0.54 | 54 | 185273 | 9.3 |
| java | 100000 | 5.12 | 51 | 1850683 | 9.3 |
| java | 1000000 | 49.45 | 49 | 18454811 | 9.2 |
| go | 10000 | 0.53 | 53 | 184995 | 9.2 |
| go | 100000 | 5.33 | 53 | 1851223 | 9.3 |
| go | 1000000 | 53.99 | 54 | 18529694 | 9.3 |
| cpp | 10000 | 0.40 | 40 | 185165 | 9.3 |
| cpp | 100000 | 4.06 | 41 | 1851421 | 9.3 |
| cpp | 1000000 | 40.89 | 41 | 18513171 | 9.3 |
| c | 10000 | 0.44 | 44 | 183685 | 9.2 |
| c | 100000 | 4.41 | 44 | 1838900 | 9.2 |
| c | 1000000 | 43.74 | 44 | 18391205 | 9.2 |

- Queries are heavy: a query returns on average about 26,000 document IDs on the 256 MB corpus (`AND`/`OR` merge long lists), and the time is linear in `--ops`: 41–54 µs per query. The clock reading cost (12–27 ns) is under 0.1% of that.
- A latency line is 9.2–9.3 bytes, so a run's latency file is about `9.3 × ops × (warm-up + repetitions)` bytes: 100,000 operations with the 11 repetitions of `params.sh` give about 10 MB, the placeholder `OPS_TOTAL=1000000` would give about 100 MB per run.
- The mixed modes were **not measured**. Their cost is dominated by the inserts: indexing runs at 27,000 (Java), 40,000 (Go), 27,000 (C++) and 46,000 (C) documents per second on the 256 MB corpus (101,131 documents in `build_ns`).

**Constraint that decides the sizes:** a mixed run needs `PRELOAD_DOCS + floor(ops × insert share / 100)` documents of `QUERY_CORPUS_MB` (section Workload model), and the largest insert share is 80%. The 256 MB corpus has only 101,131 documents, so with the placeholders (`OPS_TOTAL=1000000`) `mixed-20-80` would need 850,000. A run with 100,000 operations needs 80,000 inserts plus the preload.

**Decision (applied in `params.sh`, `params-pinned.sh` takes the same values):**

| parameter | placeholder | value | reasoning |
|---|---|---|---|
| `QUERY_CORPUS_MB` | 256 | **512** | 203,212 documents (after the re-cut to base 128 MB), enough for preload + inserts (below); the index is still small (1.1–2.9 GB at the build peak). The 256 MB corpus would force a preload of at most 21,000 documents. |
| `PRELOAD_DOCS` | 50,000 | **100,000** | Half of the 512 MB corpus: the mixed runs start from a real index (about 1 GB), and the preload leaves 103,358 documents for inserts. |
| `OPS_TOTAL` | 1,000,000 | **100,000** | Strong scaling. At 1 worker: about 4–5 s per repetition at 256 MB, about 8–11 s at 512 MB (extrapolated: lists twice as long, to be confirmed by a first run), so 11 repetitions take about 2 minutes. `mixed-20-80` needs 80,000 + 100,000 = 180,000 ≤ 203,212 documents. At 22 workers a worker has 4,500 operations (about 0.2–0.5 s per repetition), still far above the clock cost. Latency files: about 10 MB per run instead of 100 MB. |
| `OPS_PER_WORKER` | 100,000 | **5,000** | Weak scaling: 16 workers do 80,000 operations, at most 64,000 inserts + 100,000 preload = 164,000 ≤ 203,212; same work per worker as strong scaling at about 20 workers. 10,000 would need 228,000 documents at 16 workers. |
| `QUERY_COUNT` | 100,000 | **100,000** (unchanged) | Enough that no worker wraps around the query file (largest share: 100,000 operations at 1 worker, exactly once); the file is 1.45 MB. |
| `TIMEOUT` | 30m | **60m** (for now) | With 2048 MB as the largest corpus the slowest sequential run is Java or C++ building it: about 36 s per build, about 7 minutes for 11 repetitions. 60m is about 9× that: generous on purpose, because a concurrent variant at 1 worker can be slower than a sequential one (locks) and a hung run is only detected at the limit. To be tightened after the first full runs (open items). |

**Decision on the weak-scaling base (taken):** weak scaling at 16 workers used a 4096 MB corpus, where Java (about 18 GB) and Go (14.3 GB) do not fit the budget. Option (a) was chosen: `CORPUS_BASE_MB=128`, `CORPORA_MB="128 256 512 1024 2048"`, so 16 workers are 2048 MB (Java 9.1 GB, Go 7.1 GB). The invalid Go run at 4096 MB and the missing Java run at 4096 MB are therefore not needed any more. The query timings above were measured on the 256 MB corpus of the earlier cut and the proposal extrapolates them to 512 MB (lists twice as long, about 8–11 s per repetition at 1 worker); the first full run confirms or corrects it.

### Run conditions

- Plugged in, performance power profile, other programs closed. The runner records the power source, swap use and CPU power profile (platform profile, governor, energy preference) in `meta.txt` before and after the sweep, and warns, without stopping, on battery power or if swap pages were moved during the sweep. The profile is recorded, not enforced; any warning is reported with the results.
- Cool-down pauses between runs and randomized run order. *Why: thin laptops throttle when hot.*
- Per-repetition rows make throttling visible as variability.

## Open items

The shared design is complete; what remains are the variant designs and numbers that need the first measurements.

- [x] Design the variants: which combinations of structural dimensions to implement, per language, and the question each one answers. Done in `docs/experiment-plan.md` (section The implementations); built in three rounds (section Order of work there).
- [ ] Visibility check for mixed modes (what a query may observe). Includes how a concurrent variant keeps posting lists sorted while they are queried, since `AND` and `OR` merge sorted lists.
- [x] Preprocessing: markup stripping is settled by using a pre-extracted plain-text source (section Corpus); `corpus/extract.sh` (documents), `corpus/normalize.sh` (tokenization), `corpus/make-prefixes.sh` (prefixes) and `corpus/make-queries.sh` (queries) are implemented.
- [x] Read the P-core and E-core CPU numbers for pinning: P-cores (with HT) are CPUs 0-11, E and LP E-cores are 12-21 (`/sys/devices/cpu_core/cpus`, `cpu_atom/cpus`).
- [x] Confirm corpus sizes and the memory budget from measured bytes per posting (section Sizing measurements). Result: base 128 MB, largest size 2048 MB (4096 MB dropped: Java and Go would not fit).
- [x] Size `--ops`, `--preload`, `QUERY_CORPUS_MB` and the query count so that runs and latency files stay manageable (section Sizing measurements): `QUERY_CORPUS_MB=512`, `PRELOAD_DOCS=100000`, `OPS_TOTAL=100000`, `OPS_PER_WORKER=5000`, `QUERY_COUNT=100000`. The runner's mixed-sizing check passes on the real corpus for all three params files. To confirm with the first full run: query cost on the 512 MB corpus (extrapolated).
- [x] Insert budget in mixed modes: exact operation counts, shuffled per worker with the shared generator (section Shared implementation rules); `check.sh` now checks mixed modes too.
- [x] Fix compiler flags, JVM heap and GC settings, `GOMAXPROCS` (section Shared implementation rules, Build and runtime settings).
- [ ] Tighten `TIMEOUT` after the first full runs.
- [x] Decide how `results/<experiment>` folders are named: each params file sets `EXPERIMENT` (`full`, `pinned`, `quick`), so dev results land in ignored folders.
