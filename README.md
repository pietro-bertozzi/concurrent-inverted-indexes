# Concurrent Inverted Index — Benchmark

Inverted index implementations in Java, Go, C++ and C, each with several concurrency
strategies, benchmarked against each other on build, query and mixed read/write workloads.
Performance only. Project for the Concurrent and Parallel Systems course.

The full design (workload, data, CLI contract, results format, methodology) is in
[`docs/design.md`](docs/design.md), which is the source of truth.

## Layout

```
bench/       runner (run.sh), correctness check (check.sh), shared code (common.sh) and
             parameter files (params.sh, params-pinned.sh, params-quick.sh, params-q1.sh to params-q5.sh, plus params-q2b.sh;
             Q4 has two: params-q4a.sh and params-q4b.sh)
corpus/      preprocessing scripts: raw Wikipedia -> corpus, queries
data/        generated corpus prefixes and query files (not committed); raw dumps in data/raw/
impl/        impl/<language>/<variant>/ with build.sh, run.sh, dimensions.tsv
             folders starting with _ hold shared code, not variants
results/     one folder per final experiment (q1 to q5, q2b): results.tsv, logs/, meta.txt; no latency files
analysis/    Python analysis (later)
docs/        design document
```

## Usage

Prepare the data once. The source is the pre-extracted English Wikipedia of TU Darmstadt
(Sept 2015, [DOI 10.5281/zenodo.229904](https://doi.org/10.5281/zenodo.229904), 4.4 GB,
see `docs/design.md`, section Corpus):

```
curl -L -o data/raw/wikipedia.txt.gz https://zenodo.org/api/records/229904/files/wikipedia.txt.gz/content
md5sum data/raw/wikipedia.txt.gz                                 # c1fefd53b73798459e8623e2b965f75f
zcat data/raw/wikipedia.txt.gz | corpus/extract.sh | corpus/normalize.sh > data/raw/corpus-dump-order.txt
corpus/shuffle.sh bench/params.sh < data/raw/corpus-dump-order.txt > data/raw/corpus.txt   # seeded shuffle
corpus/make-prefixes.sh bench/params.sh data/raw/corpus.txt      # data/corpus-<MB>mb.txt
corpus/make-queries.sh bench/params.sh data/corpus-512mb.txt     # data/queries.txt, from the first PRELOAD_DOCS documents
```

Run the benchmark:

```
./bench/run.sh bench/params.sh          every variant, every mode (11,547 runs: too long to run in one sitting, not run)
./bench/run.sh bench/params-pinned.sh   the same pinned to the P-cores (722 runs, not run: Q4a is the pinned control)
./bench/run.sh bench/params-quick.sh    small sweep for development
./bench/run.sh bench/params-q1.sh       experiment Q1 (build, 128 MB, locks and batching): 292 runs, 59 minutes measured
./bench/run.sh bench/params-q2.sh       experiment Q2 (sharing or replicating, strong and weak scaling): 272 runs, 115 minutes measured (over the one-hour rule, run overnight)
./bench/run.sh bench/params-q2b.sh      experiment Q2 follow-up (does the merge algorithm decide the answer? `private-merge-heap`, build, 128 MB): 36 runs, 6 minutes measured
./bench/run.sh bench/params-q3.sh       experiment Q3 (locks or messages, build, 128 MB): 292 runs, 61 minutes measured
./bench/run.sh bench/params-q4a.sh      experiment Q4, pinned control (P-cores only, up to 12 workers): 58 runs, 9.5 minutes measured
./bench/run.sh bench/params-q4b.sh      experiment Q4, static against dynamic handout, unpinned: 44 runs, 7 minutes measured
./bench/run.sh bench/params-q5.sh       experiment Q5 (read/write lock, query and mixed modes, Java and C, 512 MB): 136 runs, 58 minutes measured
```

Check that every variant builds the right index (dump of the test data in `data/tiny/`
compared with `cmp`, in every mode the variant declares; mixed modes are compared with the
variant's own build-mode dump of the same corpus prefix; prints one PASS/FAIL row per
variant, exit code 0 only if all pass):

```
./bench/check.sh                        uses bench/params-quick.sh
./bench/check.sh bench/params.sh        worker counts of another params file
```

The run shape and the numeric parameters passed to variants by `check.sh` are the `CHECK_*`
variables of the params file (including `CHECK_OPS` and `CHECK_PRELOAD` for query and mixed
modes).

Before the sweep the runner checks that the data files exist and that the mixed modes fit
in the corpus (`QUERY_CORPUS_MB`); it records power source, swap and CPU profile in
`meta.txt` and warns on battery power or swap use. A mixed run inserts exactly
`floor(ops × insert share / 100)` documents, so it needs exactly `preload` plus that many.
It also warns if a language has no
`sequential` variant. Runs that differ only in scaling run once, with `scaling` =
`strong,weak`. In weak scaling a variant whose `max_threads` is below N still runs the work
of N with `max_threads` workers; the last column of `results.tsv`, `work_n`, records which
N's work a run did (it equals `threads` in every other run). The run order is shuffled with
`SHUFFLE_SEED`.

Results land in `results/<experiment>/<timestamp>/` (`results.tsv`, `failures.tsv`,
`runlist.tsv`, `logs/`, `latencies/`, `meta.txt`). Hand-made test data (tiny corpus, queries and the
expected canonical dump) is in `data/tiny/`.

## Status

Every variant of `docs/experiment-plan.md` is implemented and passes `bench/check.sh` at every
worker count (27 variants: `sequential`, `global-lock`, `partition-lock`, `partition-writers`,
`private-merge` and `private-merge-heap` in Java, Go, C++ and C; `partition-rwlock` in Java and
C; `private-merge-static` in C). Every experiment has run, with no failures, in the plugged-in
`performance` profile with no swap in use, and is committed in `results/`: Q1 (`q1`), Q2 (`q2`,
and its follow-up on the merge algorithm, `q2b`), Q3 (`q3`), Q4 (`q4a`, `q4b`) and Q5 (`q5`);
the plan has a Results section for each question. The raw latency files of Q5 are not in the
repository (they go to a GitHub release). `results/full/` (a first sweep of the runner) is a dress
rehearsal, kept locally and excluded from the analysis (`docs/design.md`, section Layout).
Still to do: the Python analysis (`analysis/`) and the report.

Java needs `javac` and `java` (JDK 25), Go needs `go` (no `go.mod`: `build.sh`
compiles the sources from a copy in `build/src/`), C++ needs `g++`, C needs `gcc` (its hash map is our own, see `docs/design.md`). The corpus
sizes (128 to 2048 MB) and the `OPS_*`, `PRELOAD_DOCS` and `QUERY_CORPUS_MB` values of the
params files come from the sizing measurements (`docs/design.md`, section Sizing measurements).

## License

See [`LICENSE`](LICENSE).
