# Numeric parameters of experiment Q3 (docs/experiment-plan.md): locks or messages?
# partition-writers (owner per partition, bounded queues) against partition-lock with the same
# P and batch, and the sequential baseline. Build mode, strong scaling, 128 MB corpus, at most
# 70 minutes estimated (docs/design.md, Run conditions). Everything not named in the plan is as
# in params-q1.sh.
#
# Numeric parameters for the full measurements. Plain bash, read with `source`.
# This file is the only place where these numbers live (docs/design.md, Dimensions).
# Lists are space-separated; the runner sweeps the cross product, restricted to what
# each variant declares in its dimensions.tsv (`modes` and `uses`).

# Name of the experiment: results go to results/$EXPERIMENT/<timestamp>/.
EXPERIMENT=q3

# Workload modes.
MODES="build"

# Scaling kinds. strong: fixed total work, more workers. weak: work proportional to N.
SCALING="strong"

# Worker counts. Strong values include the hardware boundaries (docs/design.md, Thread values):
# 6 P-cores, 12 P-core threads, 16 cores, 22 threads; 14 (P- and E-cores) is only in params.sh.
THREADS="1 2 4 6 8 12 16 22"
THREADS_WEAK="1 2 4 8 16"

# Corpus prefixes, in MB of text (cut by token count, see corpus/make-prefixes.sh).
# Files are data/corpus-<MB>mb.txt. Only build mode scales the corpus: strong scaling
# runs on every size in CORPORA_MB, weak scaling on CORPUS_BASE_MB * N (so every such
# size must also be listed in CORPORA_MB).
CORPUS_BASE_MB=128
CORPORA_MB="128"

# The one corpus (MB, must be listed in CORPORA_MB) that query and mixed modes run on:
# they scale the number of operations instead. Sized in docs/design.md, section Sizing
# measurements: 203,212 documents, enough for PRELOAD_DOCS plus the largest insert budget.
QUERY_CORPUS_MB=128

# Variants this question needs (docs/design.md, Numeric parameters).
VARIANTS="*/sequential */partition-lock */partition-writers"

# Numeric parameters swept only for variants that list them in `uses`.
PARTITIONS="64"
BATCH="1 64 1024"
QUEUE="4 64"

# CPU placement: none = the OS schedules freely; pcores = taskset on the P-core threads.
# Runs with more workers than PCORES_CPUS has CPUs are skipped when pinned. The main
# experiment is unpinned; the pinned control experiment is bench/params-pinned.sh.
PINNING="none"
PCORES_CPUS="0-11"  # /sys/devices/cpu_core/cpus on the benchmark machine

# Operations (query and mixed modes), sized in design.md, section Sizing measurements.
# Strong scaling: OPS_TOTAL operations in total. Weak: OPS_PER_WORKER * N.
# mixed-20-80 at the largest value needs PRELOAD_DOCS + 80% of the operations in documents:
# 100,000 + 80,000 (strong) or 100,000 + 64,000 (weak, N = 16) <= 203,212 (QUERY_CORPUS_MB).
OPS_TOTAL=100000
OPS_PER_WORKER=5000

# Documents indexed before timing starts (mixed modes). The queries are drawn from these
# same documents (corpus/make-queries.sh), so every query term is in the index at the start.
PRELOAD_DOCS=100000

# Number of queries in data/queries.txt (corpus/make-queries.sh).
QUERY_COUNT=100000

# Base seed for query selection; each worker derives its own.
SEED=42

# Seed of the one-time document shuffle (corpus/shuffle.sh) that precedes the prefix cut.
CORPUS_SEED=2015

# Repetitions per run: WARMUP extra ones run first and are recorded with warmup=1.
REPEAT=5
WARMUP=1

# Limit per run, in the format of `timeout` (60m = 60 minutes): about 9x the slowest
# sequential run (a 2048 MB build, 11 repetitions, about 7 minutes; design.md, Sizing
# measurements); to be tightened after the first full runs.
TIMEOUT=5m

# Pause in seconds before every run, to let the laptop cool down.
COOLDOWN=3

# Seed of the run-order shuffle, so that the same params file gives the same order.
SHUFFLE_SEED=1

# Correctness check (bench/check.sh): the run shape and the numeric parameters it passes to
# the variants that use them. Small values: the test corpus has 6 documents.
CHECK_REPEAT=1
CHECK_WARMUP=0
CHECK_PARTITIONS=4
CHECK_BATCH=2
CHECK_QUEUE=8

# Operations and preload of the query and mixed checks. data/tiny/ has 6 documents and 7
# queries; preload + floor(CHECK_OPS x 80 / 100) must fit in the documents.
CHECK_OPS=7
CHECK_PRELOAD=1
