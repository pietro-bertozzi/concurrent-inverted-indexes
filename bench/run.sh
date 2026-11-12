#!/usr/bin/env bash
# Benchmark runner (docs/design.md, section Benchmark runner).
#
#   ./bench/run.sh bench/params.sh          full measurements
#   ./bench/run.sh bench/params-quick.sh    small sweep for development
#
# Discovers every impl/<language>/<variant>/ folder, builds it, sweeps the parameters of
# the params file, runs each variant through its run.sh (the CLI contract), and collects
# results/<experiment>/<timestamp>/{results.tsv,failures.tsv,logs/,latencies/,meta.txt}.
# stdout is not used: all messages go to stderr.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DATA="$ROOT/data"

# Columns an implementation prints on stdout, in this order (design.md, Measurement
# columns). Implementations print data rows only; the runner owns the header.
MEASURE_COLS=(rep warmup load_ns preload_ns build_ns run_ns finalize_ns ops_query
              ops_insert docs terms postings peak_rss_bytes cpu_user_ns cpu_sys_ns
              ctx_vol ctx_invol)

# Numeric parameters a variant can ask for in `uses` (CLI: --partitions, --batch, --queue).
USABLE_PARAMS=(partitions batch queue)

# Variables every params file must define.
REQUIRED_PARAMS=(EXPERIMENT MODES SCALING THREADS THREADS_WEAK CORPUS_BASE_MB CORPORA_MB
                 PARTITIONS BATCH QUEUE PINNING PCORES_CPUS OPS_TOTAL OPS_PER_WORKER
                 PRELOAD_DOCS SEED REPEAT WARMUP TIMEOUT COOLDOWN SHUFFLE_SEED)

VARIANTS=()                 # discovered variants, as "language/variant"
declare -A DIM=()           # DIM["language/variant|key"]=value, from dimensions.tsv
DIM_KEYS=()                 # union of all dimension keys, sorted
BUILT=()                    # variants whose build succeeded
declare -A BUILD_SETTINGS=()  # compiler/JVM version and flags, per variant
RESULT=""                   # results/<experiment>/<timestamp>
FAILED=0                    # number of failed runs and builds

# Prints a message to stderr.
log() { echo "run.sh: $*" >&2; }

# Prints an error to stderr and stops.
die() { echo "run.sh: error: $*" >&2; exit 1; }

# Prints the arguments joined with tabs.
join_tabs() { local IFS=$'\t'; echo "$*"; }

# Succeeds if $1 is one of the comma-separated items in $2.
in_csv() { [[ ",$2," == *",$1,"* ]]; }

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

# Checks the command line, the tools and the params file, then sources it.
load_params() {
    [[ $# -eq 1 ]] || die "usage: bench/run.sh <params-file> (e.g. bench/params.sh)"
    [[ -f "$1" ]] || die "params file '$1' does not exist"
    local tool name value
    for tool in timeout taskset shuf awk; do
        command -v "$tool" >/dev/null || die "required tool '$tool' not found in PATH"
    done
    PARAMS_FILE="$(realpath "$1")"
    # shellcheck source=/dev/null
    source "$PARAMS_FILE"
    for name in "${REQUIRED_PARAMS[@]}"; do
        [[ -n "${!name+set}" ]] || die "$PARAMS_FILE does not define $name"
    done
    for name in CORPUS_BASE_MB OPS_TOTAL OPS_PER_WORKER PRELOAD_DOCS SEED REPEAT \
                COOLDOWN SHUFFLE_SEED; do
        [[ "${!name}" =~ ^[0-9]+$ ]] || die "$name must be a non-negative integer, got '${!name}'"
    done
    for value in $MODES; do
        mode_kind "$value" >/dev/null || die "unknown mode '$value' in MODES"
    done
    for value in $SCALING; do
        [[ "$value" == strong || "$value" == weak ]] || die "unknown scaling '$value'"
    done
    for value in $PINNING; do
        [[ "$value" == none || "$value" == pcores ]] || die "unknown pinning '$value'"
    done
}

# Fills VARIANTS with every impl/<language>/<variant>/ folder; folders starting with _
# are shared code. A variant missing build.sh, run.sh or dimensions.tsv is an error.
discover_variants() {
    local lang_dir var_dir lang variant
    for lang_dir in "$ROOT"/impl/*/; do
        lang="$(basename "$lang_dir")"
        [[ "$lang" != _* ]] || continue
        for var_dir in "$lang_dir"*/; do
            [[ -d "$var_dir" ]] || continue
            variant="$(basename "$var_dir")"
            [[ "$variant" != _* ]] || continue
            [[ -x "$var_dir/build.sh" ]] || die "$lang/$variant: build.sh missing or not executable"
            [[ -x "$var_dir/run.sh" ]] || die "$lang/$variant: run.sh missing or not executable"
            [[ -f "$var_dir/dimensions.tsv" ]] || die "$lang/$variant: dimensions.tsv missing"
            VARIANTS+=("$lang/$variant")
        done
    done
    [[ ${#VARIANTS[@]} -gt 0 ]] || die "no variants found under $ROOT/impl/<language>/<variant>/"
    log "discovered ${#VARIANTS[@]} variants: ${VARIANTS[*]}"
}

# Reads every dimensions.tsv into DIM and builds DIM_KEYS, the sorted union of the keys.
# Each line is "key<TAB>value"; blank lines and lines starting with # are ignored.
read_dimensions() {
    local id key value line_no
    local -a all_keys=()
    for id in "${VARIANTS[@]}"; do
        line_no=0
        while IFS=$'\t' read -r key value || [[ -n "$key" ]]; do
            line_no=$(( line_no + 1 ))
            [[ -n "$key" && "$key" != \#* ]] || continue
            [[ -n "$value" ]] || die "$id/dimensions.tsv line $line_no: key '$key' has no value (expected key<TAB>value)"
            [[ -z "${DIM[$id|$key]+set}" ]] || die "$id/dimensions.tsv line $line_no: duplicate key '$key'"
            DIM["$id|$key"]="${value//$'\t'/ }"
            all_keys+=("$key")
        done < "$ROOT/impl/$id/dimensions.tsv"
        [[ -n "${DIM[$id|uses]+set}" ]] || die "$id/dimensions.tsv has no 'uses' key"
        [[ -n "${DIM[$id|modes]+set}" ]] || die "$id/dimensions.tsv has no 'modes' key"
        check_variant_lists "$id"
    done
    mapfile -t DIM_KEYS < <(printf '%s\n' "${all_keys[@]}" | sort -u)
}

# Checks that the `uses` and `modes` of a variant contain only known names.
check_variant_lists() {
    local id="$1" item
    local -a items
    IFS=, read -ra items <<<"${DIM[$id|uses]}"
    for item in "${items[@]}"; do
        [[ "$item" == none ]] && continue
        printf '%s\n' "${USABLE_PARAMS[@]}" | grep -qx "$item" \
            || die "$id: unknown parameter '$item' in uses (known: ${USABLE_PARAMS[*]}, or none)"
    done
    IFS=, read -ra items <<<"${DIM[$id|modes]}"
    for item in "${items[@]}"; do
        mode_kind "$item" >/dev/null || die "$id: unknown mode '$item' in modes"
    done
}

# Prints the value of a dimension of a variant, or "-" if it does not declare it.
dim() { echo "${DIM[$1|$2]:--}"; }

# Creates results/<experiment>/<timestamp>/ with its subfolders and the two headers.
make_result_dir() {
    RESULT="$ROOT/results/$EXPERIMENT/$(date +%Y%m%d-%H%M%S)"
    mkdir -p "$RESULT/logs" "$RESULT/latencies"
    local -a header=(timestamp commit host cpu cores language variant "${DIM_KEYS[@]}"
                     mode corpus threads scaling partitions batch queue pinning ops preload
                     seed build_settings run "${MEASURE_COLS[@]}")
    join_tabs "${header[@]}" > "$RESULT/results.tsv"
    join_tabs run phase status variant mode scaling corpus_mb threads partitions batch queue \
        pinning ops preload > "$RESULT/failures.tsv"
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

# Writes meta.txt: machine, commit, tool versions and a copy of the params file.
write_meta() {
    {
        echo "started: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
        echo "commit: $COMMIT"
        echo "host: $HOST"
        echo "cpu: $CPU"
        echo "cores: $CORES"
        echo "kernel: $(uname -sr)"
        echo "memory: $(awk '/MemTotal/ {print $2 " kB"}' /proc/meminfo)"
        echo "bash: $BASH_VERSION"
        echo "timeout: $(timeout --version | head -n1)"
        echo "params_file: $PARAMS_FILE"
        echo "variants: ${VARIANTS[*]}"
        echo
        echo "----- params file -----"
        cat "$PARAMS_FILE"
    } > "$RESULT/meta.txt"
}

# Runs build.sh of every variant (log in logs/). A failed build is recorded and the
# variant is skipped. Reads build/settings.txt, written by build.sh, for the
# build_settings column ("-" if absent).
build_all() {
    local id settings_file
    for id in "${VARIANTS[@]}"; do
        log "building $id"
        if (cd "$ROOT/impl/$id" && ./build.sh) > "$RESULT/logs/build-${id//\//-}.log" 2>&1; then
            BUILT+=("$id")
            settings_file="$ROOT/impl/$id/build/settings.txt"
            if [[ -s "$settings_file" ]]; then
                BUILD_SETTINGS["$id"]="$(head -n1 "$settings_file" | tr '\t' ' ')"
            else
                BUILD_SETTINGS["$id"]="-"
            fi
        else
            log "build of $id FAILED, variant skipped (see logs/build-${id//\//-}.log)"
            join_tabs - build failed "$id" - - - - - - - - - - >> "$RESULT/failures.tsv"
            FAILED=$(( FAILED + 1 ))
        fi
    done
    [[ ${#BUILT[@]} -gt 0 ]] || die "no variant built successfully"
}

# Prints, one per line, the runs of one variant and mode as tab-separated fields:
#   variant mode scaling corpus_mb threads partitions batch queue pinning ops preload
# The runs are the cross product of scaling x threads x corpus x the parameters in
# `uses` x pinning. "-" marks a field that does not apply.
runs_for() {
    local id="$1" mode="$2" kind scaling n mb p b q pin ops preload
    local -a threads corpora parts batches queues
    local uses="${DIM[$id|uses]}" pcores_count
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
            if [[ "$scaling" == weak && "$kind" != mixed ]]; then
                corpora=($(( CORPUS_BASE_MB * n )))   # weak: corpus grows with N
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

# Builds the list of runs of all built variants, checks that the data files exist, and
# shuffles it (seeded, so the same params file gives the same order) into $RESULT/runlist.tsv.
make_runlist() {
    local id mode unshuffled="$RESULT/runlist.unshuffled.tsv"
    local -a missing=()
    : > "$unshuffled"
    for id in "${BUILT[@]}"; do
        for mode in $MODES; do
            in_csv "$mode" "${DIM[$id|modes]}" || continue
            runs_for "$id" "$mode" >> "$unshuffled"
        done
    done
    [[ -s "$unshuffled" ]] || die "the run list is empty: check MODES and the modes/uses of the variants"
    local file
    while IFS= read -r file; do
        [[ -f "$file" ]] || missing+=("$file")
    done < <(cut -f4 "$unshuffled" | sort -u | sed "s|.*|$DATA/corpus-&mb.txt|")
    if cut -f2 "$unshuffled" | grep -qE '^(query|mixed-)' && [[ ! -f "$DATA/queries.txt" ]]; then
        missing+=("$DATA/queries.txt")
    fi
    if [[ ${#missing[@]} -gt 0 ]]; then
        die "missing data files (see corpus/): ${missing[*]}"
    fi
    shuf --random-source=<(yes "$SHUFFLE_SEED") "$unshuffled" > "$RESULT/runlist.tsv"
    rm "$unshuffled"
    log "$(wc -l < "$RESULT/runlist.tsv") runs"
}

# Runs one entry of the run list: builds the command line, runs it under timeout (and
# taskset if pinned), checks the rows it printed, and appends them with the context
# columns. A failure is recorded in failures.tsv and the sweep continues.
run_one() {
    local spec="$1" number="$2" total="$3"
    local id mode scaling mb threads parts batch queue pin ops preload
    IFS=$'\t' read -r id mode scaling mb threads parts batch queue pin ops preload <<<"$spec"
    local run kind lang variant out logfile status=0 context row
    run="$(printf '%04d' "$number")"
    kind="$(mode_kind "$mode")"
    lang="${id%%/*}"; variant="${id#*/}"
    out="$RESULT/logs/$run.stdout"; logfile="$RESULT/logs/$run.log"

    local -a args=(--mode "$mode" --input "$DATA/corpus-${mb}mb.txt" --threads "$threads"
                   --repeat "$REPEAT" --warmup "$WARMUP")
    if [[ "$kind" != build ]]; then
        args+=(--queries "$DATA/queries.txt" --ops "$ops" --seed "$SEED"
               --latencies "$RESULT/latencies/$run.tsv")
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

    local -a dims=()
    local key
    for key in "${DIM_KEYS[@]}"; do dims+=("$(dim "$id" "$key")"); done
    context="$(join_tabs "$context" "$COMMIT" "$HOST" "$CPU" "$CORES" "$lang" "$variant" \
        "${dims[@]}" "$mode" "${mb}mb" "$threads" "$scaling" "$parts" "$batch" "$queue" \
        "$pin" "$ops" "$preload" "$([[ "$kind" == build ]] && echo - || echo "$SEED")" \
        "${BUILD_SETTINGS[$id]}" "$run")"
    while IFS= read -r row; do
        printf '%s\t%s\n' "$context" "$row"
    done < "$out" >> "$RESULT/results.tsv"
}

# Records a failed run in failures.tsv and in the log, and counts it.
fail_run() {
    local run="$1" phase="$2" status="$3" spec="$4" id rest
    id="${spec%%$'\t'*}"; rest="${spec#*$'\t'}"
    log "run $run FAILED ($phase, status $status), see logs/$run.log"
    join_tabs "$run" "$phase" "$status" "$id" "$rest" >> "$RESULT/failures.tsv"
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
    CORES="$(nproc --all)"
    discover_variants
    read_dimensions
    make_result_dir
    write_meta
    build_all
    make_runlist
    run_all
    echo "finished: $(date -u +%Y-%m-%dT%H:%M:%SZ)" >> "$RESULT/meta.txt"
    log "done: $FAILED failure(s); results in $RESULT"
}

main "$@"
