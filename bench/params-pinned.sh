# Control experiment: the same measurements as params.sh, but pinned to the P-core threads
# (taskset), on a smaller sweep. If the bend of the speedup curve at 6 workers disappears
# here, it was the hardware, not contention (docs/design.md, section Pinning).
# Every value is a subset of the values in params.sh, so the rows can be compared directly.
# Results go to results/pinned/<timestamp>/.

EXPERIMENT=pinned

MODES="build query mixed-50-50"
SCALING="strong"

# Only worker counts that fit on the P-core threads (CPUs 0-11); weak scaling is not
# repeated here, so THREADS_WEAK is only a required placeholder.
THREADS="1 2 4 6 8 12"
THREADS_WEAK="1 2 4 8"

# Build runs on two corpus sizes; query and mixed on QUERY_CORPUS_MB, as in params.sh.
CORPUS_BASE_MB=256
CORPORA_MB="256 1024"
QUERY_CORPUS_MB=256

# One value each: the control is about the thread count, not about these parameters.
PARTITIONS="16"
BATCH="64"
QUEUE="1024"

PINNING="pcores"
PCORES_CPUS="0-11"

OPS_TOTAL=1000000
OPS_PER_WORKER=100000
PRELOAD_DOCS=50000
QUERY_COUNT=100000
SEED=42

REPEAT=10
WARMUP=1
TIMEOUT=30m
COOLDOWN=10
SHUFFLE_SEED=1
