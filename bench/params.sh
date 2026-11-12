# Numeric parameters for the full measurements. Plain bash, read with `source`.
# This file is the only place where these numbers live (docs/design.md, Dimensions).
# Lists are space-separated; the runner sweeps the cross product, restricted to what
# each variant declares in its dimensions.tsv (`modes` and `uses`).

# Name of the experiment: results go to results/$EXPERIMENT/<timestamp>/.
EXPERIMENT=full

# Workload modes.
MODES="build query mixed-95-5 mixed-50-50 mixed-20-80"

# Scaling kinds. strong: fixed total work, more workers. weak: work proportional to N.
SCALING="strong weak"

# Worker counts. Strong values include the hardware boundaries (6, 14, 16, 22).
THREADS="1 2 4 6 8 12 14 16 22"
THREADS_WEAK="1 2 4 8 16"

# Corpus prefixes, in MB of text (cut by token count, see corpus/make-prefixes.sh).
# Files are data/corpus-<MB>mb.txt. Weak scaling for build and query uses the corpus
# CORPUS_BASE_MB * N, so every such size must also be listed in CORPORA_MB.
CORPUS_BASE_MB=256
CORPORA_MB="256 512 1024 2048 4096"

# Numeric parameters swept only for variants that list them in `uses`.
PARTITIONS="4 16 64"
BATCH="1 64 1024"
QUEUE="1024"        # queue capacity: placeholder, to be sized (design.md, Open items)

# CPU placement: none = the OS schedules freely; pcores = taskset on the P-core threads.
# Runs with more workers than PCORES_CPUS has CPUs are skipped when pinned.
PINNING="none pcores"
PCORES_CPUS="0-11"  # /sys/devices/cpu_core/cpus on the benchmark machine

# Operations (query and mixed modes). Placeholders, to be sized (design.md, Open items).
# Strong scaling: OPS_TOTAL operations in total. Weak: OPS_PER_WORKER * N.
OPS_TOTAL=1000000
OPS_PER_WORKER=100000

# Documents indexed before timing starts (mixed modes). Placeholder, to be sized.
PRELOAD_DOCS=50000

# Number of queries in data/queries.txt (corpus/make-queries.sh). Placeholder, to be sized.
QUERY_COUNT=100000

# Base seed for query selection; each worker derives its own.
SEED=42

# Repetitions per run: WARMUP extra ones run first and are recorded with warmup=1.
REPEAT=10
WARMUP=1

# Limit per run, in the format of `timeout` (30m = 30 minutes).
TIMEOUT=30m

# Pause in seconds before every run, to let the laptop cool down.
COOLDOWN=10

# Seed of the run-order shuffle, so that the same params file gives the same order.
SHUFFLE_SEED=1
