# Concurrent Inverted Index — Benchmark

Inverted index implementations in Java, Go, C++ and C, each with several concurrency
strategies, benchmarked against each other on build, query and mixed read/write workloads.
Performance only. Project for the Concurrent and Parallel Systems course.

The full design (workload, data, CLI contract, results format, methodology) is in
[`docs/design.md`](docs/design.md), which is the source of truth.

## Layout

```
bench/       runner (run.sh) and parameter files (params*.sh)
corpus/      preprocessing scripts: raw Wikipedia -> corpus, queries
data/        generated corpus prefixes and query files (not committed); raw dumps in data/raw/
impl/        impl/<language>/<variant>/ with build.sh, run.sh, dimensions.tsv
             folders starting with _ hold shared code, not variants
results/     one folder per experiment: results.tsv, logs/, meta.txt
analysis/    Python analysis (later)
docs/        design document
```

## Usage

Not available yet (the runner is step 3 of the infrastructure phase):

```
./bench/run.sh bench/params.sh          full measurements
./bench/run.sh bench/params-quick.sh    small sweep for development
```

## Status

Infrastructure phase: repository skeleton done; no index implementations yet.

## License

See [`LICENSE`](LICENSE).
