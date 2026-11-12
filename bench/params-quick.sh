# Small sweep for development: same names as params.sh, tiny values, a few minutes at most.
# Results go to results/quick/ (ignored by git). Needs small corpus prefixes, made with
# corpus/make-prefixes.sh using this file.

EXPERIMENT=quick

MODES="build query mixed-95-5 mixed-50-50 mixed-20-80"
SCALING="strong weak"

THREADS="1 2 4"
THREADS_WEAK="1 2"

CORPUS_BASE_MB=4
CORPORA_MB="4 8"

PARTITIONS="4 16"
BATCH="1 64"
QUEUE="1024"

PINNING="none pcores"
PCORES_CPUS="0-11"

OPS_TOTAL=10000
OPS_PER_WORKER=5000

PRELOAD_DOCS=1000

QUERY_COUNT=1000

SEED=42

REPEAT=2
WARMUP=1

TIMEOUT=2m

COOLDOWN=0

SHUFFLE_SEED=1
