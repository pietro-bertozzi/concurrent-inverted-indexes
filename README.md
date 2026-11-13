# Concurrent Inverted Index — Benchmark

Inverted index implementations in Java, Go, C++ and C, each with several concurrency
strategies, benchmarked against each other on build, query and mixed read/write workloads.
Performance only. Project for the Concurrent and Parallel Systems course.

The full design (workload, data, CLI contract, results format, methodology) is in
[`docs/design.md`](docs/design.md), which is the source of truth.

## Layout

```
bench/       runner (run.sh), correctness check (check.sh), shared code (common.sh) and
             parameter files (params.sh, params-pinned.sh, params-quick.sh)
corpus/      preprocessing scripts: raw Wikipedia -> corpus, queries
data/        generated corpus prefixes and query files (not committed); raw dumps in data/raw/
impl/        impl/<language>/<variant>/ with build.sh, run.sh, dimensions.tsv
             folders starting with _ hold shared code, not variants
results/     one folder per experiment: results.tsv, logs/, meta.txt
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
zcat data/raw/wikipedia.txt.gz | corpus/extract.sh | corpus/normalize.sh > data/raw/corpus.txt
corpus/make-prefixes.sh bench/params.sh data/raw/corpus.txt      # data/corpus-<MB>mb.txt
corpus/make-queries.sh bench/params.sh data/corpus-256mb.txt     # data/queries.txt
```

Run the benchmark:

```
./bench/run.sh bench/params.sh          main experiment (unpinned)
./bench/run.sh bench/params-pinned.sh   control experiment pinned to the P-cores
./bench/run.sh bench/params-quick.sh    small sweep for development
```

Check that every variant builds the right index (dump of the test data in `data/tiny/`
compared with `cmp`; prints one PASS/FAIL row per variant, exit code 0 only if all pass):

```
./bench/check.sh                        uses bench/params-quick.sh
./bench/check.sh bench/params.sh        worker counts of another params file
```

The run shape and the numeric parameters passed to variants by `check.sh` are the `CHECK_*`
variables of the params file. Mixed modes are not checked yet (see the insert-budget open
item in `docs/design.md`).

Before the sweep the runner checks that the data files exist and that the mixed modes fit
in the corpus (`QUERY_CORPUS_MB`); it records power source, swap and CPU profile in
`meta.txt` and warns on battery power or swap use. It also warns if a language has no
`sequential` variant. Runs that differ only in scaling run once, with `scaling` =
`strong,weak`. In weak scaling a variant whose `max_threads` is below N still runs the work
of N with `max_threads` workers; the last column of `results.tsv`, `work_n`, records which
N's work a run did (it equals `threads` in every other run). The run order is shuffled with
`SHUFFLE_SEED`.

Results land in `results/<experiment>/<timestamp>/` (`results.tsv`, `failures.tsv`,
`runlist.tsv`, `logs/`, `latencies/`, `meta.txt`). Hand-made test data (tiny corpus, queries and the
expected canonical dump) is in `data/tiny/`.

## Status

Infrastructure phase: skeleton, parameters, runner, test data and preprocessing scripts are
done; there are no index implementations yet. The runner has been tested only against
throwaway fake variants. The `OPS_*`, `PRELOAD_DOCS` and `QUERY_CORPUS_MB` values in
`params.sh` are placeholders and will not pass the runner's mixed-sizing check on the real
corpus until they are sized.

## License

See [`LICENSE`](LICENSE).
