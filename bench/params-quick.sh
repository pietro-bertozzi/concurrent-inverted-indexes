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
QUERY_CORPUS=8      # query and mixed modes run on this one corpus

PARTITIONS="4 16"
BATCH="1 64"
QUEUE="1024"

PINNING="none pcores"   # both, so that development also exercises the taskset path
PCORES_CPUS="0-11"

# Small enough that preload + inserts fit in the QUERY_CORPUS (the runner checks this).
OPS_TOTAL=1000
OPS_PER_WORKER=500

PRELOAD_DOCS=100

QUERY_COUNT=1000

SEED=42

REPEAT=2
WARMUP=1

TIMEOUT=2m

COOLDOWN=0

SHUFFLE_SEED=1
