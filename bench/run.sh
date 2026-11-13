#!/usr/bin/env bash
# Benchmark runner (docs/design.md, section Benchmark runner).
#
#   ./bench/run.sh bench/params.sh          full measurements
#   ./bench/run.sh bench/params-pinned.sh   pinned control experiment
#   ./bench/run.sh bench/params-quick.sh    small sweep for development
#
# Discovers every impl/<language>/<variant>/ folder, builds it, sweeps the parameters of
# the params file, runs each variant through its run.sh (the CLI contract), and collects
# results/<experiment>/<timestamp>/{results.tsv,failures.tsv,runlist.tsv,logs/,latencies/,meta.txt}.
# stdout is not used: all messages go to stderr.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DATA="$ROOT/data"

# Columns an implementation prints on stdout, in this order (design.md, Measurement
# columns). Implementations print data rows only; the runner owns the header.
MEASURE_COLS=(rep warmup load_ns preload_ns build_ns run_ns finalize_ns ops_query
              ops_insert docs terms postings peak_rss_bytes cpu_user_ns cpu_sys_ns
              ctx_vol ctx_invol)

# Context columns of results.tsv, in order (design.md, Context columns). The dimension
# columns (one per key of the dimensions.tsv files) go between HEAD_COLS and PARAM_COLS.
HEAD_COLS=(timestamp commit host cpu cpu_cores cpu_logical language variant)
PARAM_COLS=(mode corpus_mb threads scaling partitions batch queue pinning ops preload seed)
TAIL_COLS=(build_settings run)

# Keys of dimensions.tsv that are instructions to the runner, not dimensions: they are
# never result columns.
RUNNER_KEYS=(uses modes max_threads)

# Numeric parameters a variant can ask for in `uses` (CLI: --partitions, --batch, --queue).
USABLE_PARAMS=(partitions batch queue)

# Variables every params file must define.
REQUIRED_PARAMS=(EXPERIMENT MODES SCALING THREADS THREADS_WEAK CORPUS_BASE_MB CORPORA_MB
                 QUERY_CORPUS PARTITIONS BATCH QUEUE PINNING PCORES_CPUS OPS_TOTAL
                 OPS_PER_WORKER PRELOAD_DOCS SEED REPEAT WARMUP TIMEOUT COOLDOWN
                 SHUFFLE_SEED)

VARIANTS=()                 # discovered variants, as "language/variant"
SKIPPED=()                  # variants skipped at discovery, as "language/variant|reason"
declare -A DIM=()           # DIM["language/variant|key"]=value, from dimensions.tsv
DIM_KEYS=()                 # union of all dimension keys (runner keys excluded), sorted
BUILT=()                    # variants whose build succeeded
declare -A BUILD_SETTINGS=()  # compiler/JVM version and flags, per variant
RESULT=""                   # results/<experiment>/<timestamp>
FAILED=0                    # number of failed runs, builds and skipped variants

# Prints a message to stderr.
log() { echo "run.sh: $*" >&2; }

# Prints a warning to stderr.
warn() { echo "run.sh: WARNING: $*" >&2; }

# Prints an error to stderr and stops.
die() { echo "run.sh: error: $*" >&2; exit 1; }

# Prints the arguments joined with tabs.
join_tabs() { local IFS=$'\t'; echo "$*"; }

# Succeeds if $1 is one of the comma-separated items in $2.
in_csv() { [[ ",$2," == *",$1,"* ]]; }

# Succeeds if $1 is equal to one of the remaining arguments.
in_list() {
    local wanted="$1" item
    shift
    for item in "$@"; do
        [[ "$item" == "$wanted" ]] && return 0
    done
    return 1
}

# Prints the kind of a mode: build, query or mixed (fails for an unknown mode).
mode_kind() {
    case "$1" in
        build) echo build ;;
        query) echo query ;;
        mixed-*) echo mixed ;;
        *) return 1 ;;
    esac
}

# Counts the CPUs in a list such as "0-11" or "0-3,8,10-11".
count_cpus() {
    local total=0 part
    local -a parts
    IFS=, read -ra parts <<<"$1"
    for part in "${parts[@]}"; do
        if [[ "$part" == *-* ]]; then
            total=$(( total + ${part#*-} - ${part%-*} + 1 ))
        else
            total=$(( total + 1 ))
        fi
    done
    echo "$total"
}

# Checks that the variable named $1 holds a non-empty space-separated list of positive integers.
check_int_list() {
    local name="$1" value
    [[ -n "${!name// /}" ]] || die "$name is empty"
    for value in ${!name}; do
        [[ "$value" =~ ^[1-9][0-9]*$ ]] || die "$name must contain positive integers, got '$value'"
    done
}

# Checks the command line, the tools and the params file, then sources it.
load_params() {
    [[ $# -eq 1 ]] || die "usage: bench/run.sh <params-file> (e.g. bench/params.sh)"
    [[ -f "$1" ]] || die "params file '$1' does not exist"
    local tool name value n
    for tool in timeout taskset awk sed sort cut wc; do
        command -v "$tool" >/dev/null || die "required tool '$tool' not found in PATH"
    done
    PARAMS_FILE="$(realpath "$1")"
    # shellcheck source=/dev/null
    source "$PARAMS_FILE"
    for name in "${REQUIRED_PARAMS[@]}"; do
        [[ -n "${!name+set}" ]] || die "$PARAMS_FILE does not define $name"
    done
    [[ "$EXPERIMENT" =~ ^[A-Za-z0-9._-]+$ ]] || die "EXPERIMENT must be a plain folder name, got '$EXPERIMENT'"
    for name in CORPUS_BASE_MB QUERY_CORPUS OPS_TOTAL OPS_PER_WORKER PRELOAD_DOCS SEED \
                REPEAT WARMUP COOLDOWN SHUFFLE_SEED; do
        [[ "${!name}" =~ ^[0-9]+$ ]] || die "$name must be a non-negative integer, got '${!name}'"
    done
    (( REPEAT >= 1 )) || die "REPEAT must be at least 1"
    (( CORPUS_BASE_MB >= 1 )) || die "CORPUS_BASE_MB must be at least 1"
    for name in THREADS THREADS_WEAK CORPORA_MB PARTITIONS BATCH QUEUE; do
        check_int_list "$name"
    done
    [[ "$TIMEOUT" =~ ^[0-9]+(\.[0-9]+)?[smhd]?$ ]] \
        || die "TIMEOUT must be a number with an optional s, m, h or d suffix (e.g. 30m), got '$TIMEOUT'"
    [[ "$PCORES_CPUS" =~ ^[0-9]+(-[0-9]+)?(,[0-9]+(-[0-9]+)?)*$ ]] \
        || die "PCORES_CPUS must be a CPU list such as 0-11, got '$PCORES_CPUS'"
    for value in $MODES; do
        mode_kind "$value" >/dev/null || die "unknown mode '$value' in MODES"
    done
    for value in $SCALING; do
        [[ "$value" == strong || "$value" == weak ]] || die "unknown scaling '$value'"
    done
    for value in $PINNING; do
        [[ "$value" == none || "$value" == pcores ]] || die "unknown pinning '$value'"
    done
    in_list "$CORPUS_BASE_MB" $CORPORA_MB || die "CORPUS_BASE_MB=$CORPUS_BASE_MB must be listed in CORPORA_MB"
    in_list "$QUERY_CORPUS" $CORPORA_MB || die "QUERY_CORPUS=$QUERY_CORPUS must be listed in CORPORA_MB"
    if in_list weak $SCALING && in_list build $MODES; then
        for n in $THREADS_WEAK; do
            in_list "$(( CORPUS_BASE_MB * n ))" $CORPORA_MB \
                || die "weak scaling at $n workers needs a $(( CORPUS_BASE_MB * n )) MB corpus: add it to CORPORA_MB"
        done
    fi
}

# Fills VARIANTS with every impl/<language>/<variant>/ folder; folders starting with _
# are shared code. A variant missing build.sh, run.sh or dimensions.tsv is skipped with a
# warning (recorded in failures.tsv later, like a failed build).
discover_variants() {
    local lang_dir var_dir lang variant reason
    for lang_dir in "$ROOT"/impl/*/; do
        lang="$(basename "$lang_dir")"
        [[ "$lang" != _* ]] || continue
        for var_dir in "$lang_dir"*/; do
            [[ -d "$var_dir" ]] || continue
            variant="$(basename "$var_dir")"
            [[ "$variant" != _* ]] || continue
            reason=""
            if [[ ! -x "$var_dir/build.sh" ]]; then reason="build.sh missing or not executable"
            elif [[ ! -x "$var_dir/run.sh" ]]; then reason="run.sh missing or not executable"
            elif [[ ! -f "$var_dir/dimensions.tsv" ]]; then reason="dimensions.tsv missing"
            fi
            if [[ -n "$reason" ]]; then
                warn "skipping $lang/$variant: $reason"
                SKIPPED+=("$lang/$variant|$reason")
                continue
            fi
            VARIANTS+=("$lang/$variant")
        done
    done
    [[ ${#VARIANTS[@]} -gt 0 ]] || die "no usable variants found under $ROOT/impl/<language>/<variant>/"
    log "discovered ${#VARIANTS[@]} variants: ${VARIANTS[*]}"
}

# Reads every dimensions.tsv into DIM and builds DIM_KEYS, the sorted union of the
# dimension keys. Each line is "key<TAB>value"; blank lines and lines starting with # are
# ignored. Runner keys (uses, modes, max_threads) are stored but are not result columns;
# a dimension key equal to a result column name is an error.
read_dimensions() {
    local id key value line_no
    local -a all_keys=() reserved=("${HEAD_COLS[@]}" "${PARAM_COLS[@]}" "${TAIL_COLS[@]}" "${MEASURE_COLS[@]}")
    for id in "${VARIANTS[@]}"; do
        line_no=0
        while IFS=$'\t' read -r key value || [[ -n "$key" ]]; do
            line_no=$(( line_no + 1 ))
            [[ -n "$key" && "$key" != \#* ]] || continue
            [[ -n "$value" ]] || die "$id/dimensions.tsv line $line_no: key '$key' has no value (expected key<TAB>value)"
            [[ -z "${DIM[$id|$key]+set}" ]] || die "$id/dimensions.tsv line $line_no: duplicate key '$key'"
            DIM["$id|$key"]="${value//$'\t'/ }"
            if ! in_list "$key" "${RUNNER_KEYS[@]}"; then
                in_list "$key" "${reserved[@]}" \
                    && die "$id/dimensions.tsv line $line_no: dimension key '$key' has the same name as a result column; rename it"
                all_keys+=("$key")
            fi
        done < "$ROOT/impl/$id/dimensions.tsv"
        [[ -n "${DIM[$id|uses]+set}" ]] || die "$id/dimensions.tsv has no 'uses' key"
        [[ -n "${DIM[$id|modes]+set}" ]] || die "$id/dimensions.tsv has no 'modes' key"
        check_variant_lists "$id"
    done
    if [[ ${#all_keys[@]} -gt 0 ]]; then
        mapfile -t DIM_KEYS < <(printf '%s\n' "${all_keys[@]}" | LC_ALL=C sort -u)
    fi
}

# Checks that the `uses`, `modes` and `max_threads` of a variant are valid.
check_variant_lists() {
    local id="$1" item
    local -a items
    IFS=, read -ra items <<<"${DIM[$id|uses]}"
    for item in "${items[@]}"; do
        [[ "$item" == none ]] && continue
        in_list "$item" "${USABLE_PARAMS[@]}" \
            || die "$id: unknown parameter '$item' in uses (known: ${USABLE_PARAMS[*]}, or none)"
    done
    IFS=, read -ra items <<<"${DIM[$id|modes]}"
    for item in "${items[@]}"; do
        mode_kind "$item" >/dev/null || die "$id: unknown mode '$item' in modes"
    done
    if [[ -n "${DIM[$id|max_threads]+set}" ]]; then
        [[ "${DIM[$id|max_threads]}" =~ ^[1-9][0-9]*$ ]] \
            || die "$id: max_threads must be a positive integer, got '${DIM[$id|max_threads]}'"
    fi
}

# Prints the value of a dimension of a variant, or "-" if it does not declare it.
dim() { echo "${DIM[$1|$2]:--}"; }

# Prints one row of failures.tsv (columns as in results.tsv, see make_result_dir):
#   run phase reason status language variant mode corpus_mb threads scaling partitions batch
#   queue pinning ops preload
# Arguments: run phase reason status, then a run-list entry (variant mode scaling corpus_mb
# threads partitions batch queue pinning ops preload), or just the variant id.
failure_row() {
    local run="$1" phase="$2" reason="$3" status="$4" id="$5"
    shift 5
    if [[ $# -eq 10 ]]; then
        local mode="$1" scaling="$2" mb="$3" threads="$4"
        shift 4
        join_tabs "$run" "$phase" "$reason" "$status" "${id%%/*}" "${id#*/}" "$mode" "$mb" \
            "$threads" "$scaling" "$@"
    else
        join_tabs "$run" "$phase" "$reason" "$status" "${id%%/*}" "${id#*/}" - - - - - - - - - -
    fi
}

# Creates results/<experiment>/<timestamp>/ with its subfolders and the two headers.
make_result_dir() {
    RESULT="$ROOT/results/$EXPERIMENT/$(date +%Y%m%d-%H%M%S)"
    mkdir -p "$RESULT/logs" "$RESULT/latencies"
    join_tabs "${HEAD_COLS[@]}" "${DIM_KEYS[@]}" "${PARAM_COLS[@]}" "${TAIL_COLS[@]}" \
        "${MEASURE_COLS[@]}" > "$RESULT/results.tsv"
    join_tabs run phase reason status language variant "${PARAM_COLS[@]:0:10}" \
        > "$RESULT/failures.tsv"
}

# Records the variants skipped at discovery in failures.tsv.
record_skipped() {
    local entry id reason
    for entry in "${SKIPPED[@]}"; do
        id="${entry%%|*}"; reason="${entry#*|}"
        failure_row - discover "$reason" - "$id" >> "$RESULT/failures.tsv"
        FAILED=$(( FAILED + 1 ))
    done
}

# Prints the current commit, with -dirty if the working tree has uncommitted changes.
git_commit() {
    local commit
    commit="$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null)" || { echo unknown; return; }
    if [[ -n "$(git -C "$ROOT" status --porcelain 2>/dev/null)" ]]; then
        commit="$commit-dirty"
    fi
    echo "$commit"
}

# Prints the number of physical cores (distinct "physical id:core id" in /proc/cpuinfo),
# or "-" if the file does not say.
physical_cores() {
    awk -F': *' '/^physical id/ {p = $2} /^core id/ {seen[p ":" $2] = 1}
                 END {n = 0; for (k in seen) n++; print (n > 0 ? n : "-")}' /proc/cpuinfo
}

# Prints the power source: "ac" if a mains or USB supply is online, "battery" if supplies
# exist but none is online, "unknown" if the system lists none.
power_source() {
    local dir type found=0
    for dir in /sys/class/power_supply/*/; do
        [[ -r "$dir/type" ]] || continue
        type="$(cat "$dir/type")"
        [[ "$type" == Mains || "$type" == USB ]] || continue
        found=1
        if [[ "$(cat "$dir/online" 2>/dev/null || echo 0)" == 1 ]]; then echo ac; return; fi
    done
    if (( found )); then echo battery; else echo unknown; fi
}

# Prints the contents of file $1, or "unknown" if it cannot be read.
read_or_unknown() {
    if [[ -r "$1" ]]; then cat "$1"; else echo unknown; fi
}

# Prints the swap in use, in kB.
swap_used_kb() {
    awk '/^SwapTotal/ {t = $2} /^SwapFree/ {f = $2} END {print t - f}' /proc/meminfo
}

# Prints the pages swapped in plus out since boot (a change means swap was touched).
swap_pages_moved() {
    awk '/^pswpin|^pswpout/ {s += $2} END {print s + 0}' /proc/vmstat
}

# Prints the preflight state (power source, swap, CPU power profile) as meta.txt lines.
preflight_state() {
    echo "  power_source: $(power_source)"
    echo "  swap_used_kb: $(swap_used_kb)"
    echo "  swap_pages_moved: $(swap_pages_moved)"
    echo "  platform_profile: $(read_or_unknown /sys/firmware/acpi/platform_profile)"
    echo "  cpu_governor: $(read_or_unknown /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor)"
    echo "  cpu_energy_preference: $(read_or_unknown /sys/devices/system/cpu/cpu0/cpufreq/energy_performance_preference)"
}

# Writes the first part of meta.txt: machine, commit, shuffle seed, preflight state before
# the sweep and a copy of the params file. Warns (does not stop) on battery power.
write_meta() {
    SWAP_MOVED_BEFORE="$(swap_pages_moved)"
    {
        echo "started: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
        echo "commit: $COMMIT"
        echo "host: $HOST"
        echo "cpu: $CPU"
        echo "cpu_cores: $CPU_CORES"
        echo "cpu_logical: $CPU_LOGICAL"
        echo "kernel: $(uname -sr)"
        echo "memory: $(awk '/MemTotal/ {print $2 " kB"}' /proc/meminfo)"
        echo "bash: $BASH_VERSION"
        echo "timeout: $(timeout --version | head -n1)"
        echo "params_file: $PARAMS_FILE"
        echo "shuffle_seed: $SHUFFLE_SEED"
        echo "variants: ${VARIANTS[*]}"
        echo
        echo "preflight before the sweep:"
        preflight_state
        echo
        echo "----- params file -----"
        cat "$PARAMS_FILE"
    } > "$RESULT/meta.txt"
    if [[ "$(power_source)" == battery ]]; then
        warn "running on battery power: the machine may throttle (plug it in)"
    fi
}

# Appends the end of the sweep to meta.txt: finish time and preflight state after the
# sweep. Warns (does not stop) on battery power or if swap was used during the sweep.
finish_meta() {
    local moved_after
    moved_after="$(swap_pages_moved)"
    {
        echo
        echo "----- end of sweep -----"
        echo "finished: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
        echo "failures: $FAILED"
        echo "preflight after the sweep:"
        preflight_state
    } >> "$RESULT/meta.txt"
    if [[ "$(power_source)" == battery ]]; then
        warn "the machine is on battery power after the sweep: results may have been throttled"
    fi
    if (( moved_after > SWAP_MOVED_BEFORE )); then
        warn "swap was used during the sweep ($(( moved_after - SWAP_MOVED_BEFORE )) pages moved): memory results are unreliable"
    fi
}

# Runs build.sh of every variant (log in logs/). A failed build is recorded and the
# variant is skipped. Reads build/settings.txt, written by build.sh, for the
# build_settings column ("-" if absent; a file left by an earlier build is removed first).
build_all() {
    local id settings_file status
    for id in "${VARIANTS[@]}"; do
        log "building $id"
        settings_file="$ROOT/impl/$id/build/settings.txt"
        rm -f "$settings_file"
        status=0
        (cd "$ROOT/impl/$id" && ./build.sh) > "$RESULT/logs/build-${id//\//-}.log" 2>&1 || status=$?
        if [[ "$status" -eq 0 ]]; then
            BUILT+=("$id")
            if [[ -s "$settings_file" ]]; then
                BUILD_SETTINGS["$id"]="$(head -n1 "$settings_file" | tr '\t' ' ')"
            else
                BUILD_SETTINGS["$id"]="-"
            fi
        else
            log "build of $id FAILED (status $status), variant skipped (see logs/build-${id//\//-}.log)"
            failure_row - build build-failed "$status" "$id" >> "$RESULT/failures.tsv"
            FAILED=$(( FAILED + 1 ))
        fi
    done
    [[ ${#BUILT[@]} -gt 0 ]] || die "no variant built successfully"
}

# Prints, one per line, the runs of one variant and mode as tab-separated fields:
#   variant mode scaling corpus_mb threads partitions batch queue pinning ops preload
# The runs are the cross product of scaling x threads x corpus x the parameters in
# `uses` x pinning. Thread values above the variant's max_threads are left out. Build
# scales the corpus (all CORPORA_MB in strong scaling, CORPUS_BASE_MB x N in weak); query
# and mixed modes use the one corpus QUERY_CORPUS and scale the operations. "-" marks a
# field that does not apply.
runs_for() {
    local id="$1" mode="$2" kind scaling n mb p b q pin ops preload
    local -a threads corpora parts batches queues
    local uses="${DIM[$id|uses]}" max_threads="${DIM[$id|max_threads]:-0}" pcores_count
    kind="$(mode_kind "$mode")"
    pcores_count="$(count_cpus "$PCORES_CPUS")"
    parts=(-); batches=(-); queues=(-)
    if in_csv partitions "$uses"; then read -ra parts <<<"$PARTITIONS"; fi
    if in_csv batch "$uses"; then read -ra batches <<<"$BATCH"; fi
    if in_csv queue "$uses"; then read -ra queues <<<"$QUEUE"; fi
    for scaling in $SCALING; do
        if [[ "$scaling" == strong ]]; then read -ra threads <<<"$THREADS"
        else read -ra threads <<<"$THREADS_WEAK"; fi
        for n in "${threads[@]}"; do
            if (( max_threads > 0 && n > max_threads )); then continue; fi
            if [[ "$kind" != build ]]; then
                corpora=("$QUERY_CORPUS")
            elif [[ "$scaling" == weak ]]; then
                corpora=($(( CORPUS_BASE_MB * n )))
            else
                read -ra corpora <<<"$CORPORA_MB"
            fi
            ops=-; preload=-
            if [[ "$kind" != build ]]; then
                if [[ "$scaling" == strong ]]; then ops="$OPS_TOTAL"
                else ops=$(( OPS_PER_WORKER * n )); fi
            fi
            if [[ "$kind" == mixed ]]; then preload="$PRELOAD_DOCS"; fi
            for mb in "${corpora[@]}"; do
            for p in "${parts[@]}"; do for b in "${batches[@]}"; do for q in "${queues[@]}"; do
            for pin in $PINNING; do
                if [[ "$pin" == pcores && "$n" -gt "$pcores_count" ]]; then continue; fi
                join_tabs "$id" "$mode" "$scaling" "$mb" "$n" "$p" "$b" "$q" "$pin" \
                    "$ops" "$preload"
            done; done; done; done
            done
        done
    done
}

# Prints the arguments of stdin lines (one per line) in a pseudo-random order that depends
# only on the seed $1: every line gets a key from a Park-Miller generator (state * 48271
# mod 2^31-1, exact in awk doubles, independent of the awk implementation), then the lines
# are sorted by key (stable, so equal keys keep their input order).
shuffle_lines() {
    LC_ALL=C awk -v seed="$1" '
        function next_random() { state = (state * 48271) % 2147483647; return state }
        BEGIN {
            state = seed % 2147483647; if (state <= 0) state = 1
            for (i = 0; i < 50; i++) next_random()      # let small seeds diverge
        }
        { next_random(); printf "%d\t%s\n", next_random(), $0 }
    ' | LC_ALL=C sort -s -n -k1,1 | cut -f2-
}

# Checks that the mixed runs fit in the corpus: preload + the most inserts a run can make
# (operations x insert share, rounded up) must not exceed the documents of QUERY_CORPUS.
# Stops with an error if not. $1 is the unshuffled run list.
check_mixed_sizing() {
    local unshuffled="$1" corpus="$DATA/corpus-${QUERY_CORPUS}mb.txt" docs worst
    worst="$(awk -F'\t' '$2 ~ /^mixed-/ {
                 split($2, m, "-"); need = $11 + int(($10 * m[3] + 99) / 100)
                 if (need > max) { max = need; line = $2 " ops=" $10 " preload=" $11 } }
             END { if (max > 0) print max "\t" line }' "$unshuffled")"
    [[ -n "$worst" ]] || return 0
    docs="$(wc -l < "$corpus")"
    if (( ${worst%%$'\t'*} > docs )); then
        die "mixed modes need up to ${worst%%$'\t'*} documents (preload + inserts; worst case: ${worst#*$'\t'}), but $corpus has only $docs: lower OPS_*/PRELOAD_DOCS or raise QUERY_CORPUS"
    fi
    log "mixed sizing ok: at most ${worst%%$'\t'*} of $docs documents needed"
}

# Builds the list of runs of all built variants, checks that the data files exist and that
# the mixed runs fit in the corpus, and shuffles it (seeded with SHUFFLE_SEED, so the same
# params file gives the same order) into $RESULT/runlist.tsv.
make_runlist() {
    local id mode unshuffled="$RESULT/runlist.unshuffled.tsv" file
    local -a missing=()
    : > "$unshuffled"
    for id in "${BUILT[@]}"; do
        for mode in $MODES; do
            in_csv "$mode" "${DIM[$id|modes]}" || continue
            runs_for "$id" "$mode" >> "$unshuffled"
        done
    done
    [[ -s "$unshuffled" ]] || die "the run list is empty: check MODES and the modes/uses/max_threads of the variants"
    while IFS= read -r file; do
        [[ -f "$file" ]] || missing+=("$file")
    done < <(cut -f4 "$unshuffled" | sort -u | sed "s|.*|$DATA/corpus-&mb.txt|")
    if awk -F'\t' '$2 ~ /^(query|mixed-)/ { found = 1 } END { exit !found }' "$unshuffled" \
            && [[ ! -f "$DATA/queries.txt" ]]; then
        missing+=("$DATA/queries.txt")
    fi
    if [[ ${#missing[@]} -gt 0 ]]; then
        die "missing data files (see corpus/): ${missing[*]}"
    fi
    check_mixed_sizing "$unshuffled"
    shuffle_lines "$SHUFFLE_SEED" < "$unshuffled" > "$RESULT/runlist.tsv"
    rm "$unshuffled"
    log "$(wc -l < "$RESULT/runlist.tsv") runs"
}

# Runs one entry of the run list: builds the command line, runs it under timeout (and
# taskset if pinned), checks the rows it printed (and the latency file), and appends the
# rows with the context columns. A failure is recorded in failures.tsv and the sweep
# continues; the stdout of a failed run is kept in logs/<run>.stdout, that of a good run
# is deleted.
run_one() {
    local spec="$1" number="$2" total="$3"
    local id mode scaling mb threads parts batch queue pin ops preload
    IFS=$'\t' read -r id mode scaling mb threads parts batch queue pin ops preload <<<"$spec"
    local run kind lang variant out logfile latfile status=0 context row
    run="$(printf '%04d' "$number")"
    kind="$(mode_kind "$mode")"
    lang="${id%%/*}"; variant="${id#*/}"
    out="$RESULT/logs/$run.stdout"; logfile="$RESULT/logs/$run.log"
    latfile="$RESULT/latencies/$run.tsv"

    local -a args=(--mode "$mode" --input "$DATA/corpus-${mb}mb.txt" --threads "$threads"
                   --repeat "$REPEAT" --warmup "$WARMUP")
    if [[ "$kind" != build ]]; then
        args+=(--queries "$DATA/queries.txt" --ops "$ops" --seed "$SEED" --latencies "$latfile")
    fi
    if [[ "$kind" == mixed ]]; then args+=(--preload "$preload"); fi
    if [[ "$parts" != - ]]; then args+=(--partitions "$parts"); fi
    if [[ "$batch" != - ]]; then args+=(--batch "$batch"); fi
    if [[ "$queue" != - ]]; then args+=(--queue "$queue"); fi

    local -a cmd=(timeout --kill-after=30s "$TIMEOUT")
    if [[ "$pin" == pcores ]]; then cmd+=(taskset -c "$PCORES_CPUS"); fi
    cmd+=("$ROOT/impl/$id/run.sh" "${args[@]}")

    log "[$number/$total] $id $mode $scaling ${mb}mb threads=$threads pin=$pin" \
        "partitions=$parts batch=$batch queue=$queue"
    if (( COOLDOWN > 0 )); then sleep "$COOLDOWN"; fi

    context="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    "${cmd[@]}" > "$out" 2> "$logfile" || status=$?

    if [[ "$status" -eq 124 || "$status" -eq 137 ]]; then
        fail_run "$run" timeout "$status" "$spec"
        return
    elif [[ "$status" -ne 0 ]]; then
        fail_run "$run" exit "$status" "$spec"
        return
    fi
    # Every row must have all the measurement columns, one row per repetition + warm-up.
    if ! awk -F'\t' -v n="${#MEASURE_COLS[@]}" -v want="$(( REPEAT + WARMUP ))" \
            'NF != n { bad = 1 } END { exit (bad || NR != want) }' "$out"; then
        fail_run "$run" bad-output "$status" "$spec"
        return
    fi
    # Query and mixed runs must also have written the latency file: rep, warmup, latency_ns.
    if [[ "$kind" != build ]] && { [[ ! -s "$latfile" ]] || ! awk -F'\t' \
            'NF != 3 || $1 !~ /^[0-9]+$/ || $2 !~ /^[01]$/ || $3 !~ /^[0-9]+$/ { exit 1 }' "$latfile"; }; then
        fail_run "$run" bad-latencies "$status" "$spec"
        return
    fi

    local -a dims=()
    local key
    for key in "${DIM_KEYS[@]}"; do dims+=("$(dim "$id" "$key")"); done
    context="$(join_tabs "$context" "$COMMIT" "$HOST" "$CPU" "$CPU_CORES" "$CPU_LOGICAL" \
        "$lang" "$variant" "${dims[@]}" "$mode" "$mb" "$threads" "$scaling" "$parts" "$batch" \
        "$queue" "$pin" "$ops" "$preload" "$([[ "$kind" == build ]] && echo - || echo "$SEED")" \
        "${BUILD_SETTINGS[$id]}" "$run")"
    while IFS= read -r row; do
        printf '%s\t%s\n' "$context" "$row"
    done < "$out" >> "$RESULT/results.tsv"
    rm -f "$out"
}

# Records a failed run in failures.tsv and in the log, and counts it. $4 is the run-list entry.
fail_run() {
    local run="$1" reason="$2" status="$3" spec="$4"
    local -a fields
    IFS=$'\t' read -ra fields <<<"$spec"
    log "run $run FAILED ($reason, status $status), see logs/$run.log and logs/$run.stdout"
    failure_row "$run" run "$reason" "$status" "${fields[@]}" >> "$RESULT/failures.tsv"
    FAILED=$(( FAILED + 1 ))
}

# Runs the whole run list in order.
run_all() {
    local spec number=0 total
    total="$(wc -l < "$RESULT/runlist.tsv")"
    while IFS= read -r spec; do
        number=$(( number + 1 ))
        run_one "$spec" "$number" "$total"
    done < "$RESULT/runlist.tsv"
}

# Discovery, dimensions, build, run list, shuffle, runs.
main() {
    load_params "$@"
    COMMIT="$(git_commit)"
    HOST="$(hostname)"
    CPU="$(awk -F': ' '/model name/ {print $2; exit}' /proc/cpuinfo)"
    CPU_CORES="$(physical_cores)"
    CPU_LOGICAL="$(nproc --all)"
    discover_variants
    read_dimensions
    make_result_dir
    record_skipped
    write_meta
    build_all
    make_runlist
    run_all
    finish_meta
    log "done: $FAILED failure(s); results in $RESULT"
}

main "$@"
