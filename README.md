# Concurrent Inverted Index — Benchmark

Inverted index implementations in Java, Go, C++ and C, each with several concurrency
strategies, benchmarked against each other on build, query and mixed read/write workloads.
Performance only. Project for the Concurrent and Parallel Systems course.

The full design (workload, data, CLI contract, results format, methodology) is in
[`docs/design.md`](docs/design.md), which is the source of truth.

## Layout

```
bench/       runner (run.sh), shared code (common.sh) and
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

Prepare the data once (the wiki-markup stripping step is not decided yet, see
`docs/design.md`, Open items; the scripts below start from plain text, one document per line):

```
corpus/normalize.sh < plain.txt > data/raw/corpus.txt
corpus/make-prefixes.sh bench/params.sh data/raw/corpus.txt      # data/corpus-<MB>mb.txt
corpus/make-queries.sh bench/params.sh data/corpus-256mb.txt     # data/queries.txt
```

Run the benchmark:

```
./bench/run.sh bench/params.sh          main experiment (unpinned)
./bench/run.sh bench/params-pinned.sh   control experiment pinned to the P-cores
./bench/run.sh bench/params-quick.sh    small sweep for development
```

Before the sweep the runner checks that the data files exist and that the mixed modes fit
in the corpus (`QUERY_CORPUS_MB`); it records power source, swap and CPU profile in
`meta.txt` and warns on battery power or swap use. It also warns if a language has no
`sequential` variant. Runs that differ only in scaling run once, with `scaling` =
`strong,weak`. The run order is shuffled with `SHUFFLE_SEED`.

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
