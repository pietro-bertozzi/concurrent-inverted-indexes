#!/usr/bin/env bash
# Definitions shared by bench/run.sh and bench/check.sh: paths, the result format (column
# names), small helpers, variant discovery and the reading of dimensions.tsv. Sourced, never
# run. The filesystem is the registry: nothing here lists languages or variants
# (docs/design.md, section Repository layout and discovery).

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
# Column added by the runner after the measurement columns (new columns go at the end).
LAST_COLS=(work_n)

# Keys of dimensions.tsv that are instructions to the runner, not dimensions: they are
# never result columns.
RUNNER_KEYS=(uses modes max_threads)

# Numeric parameters a variant can ask for in `uses` (CLI: --partitions, --batch, --queue).
USABLE_PARAMS=(partitions batch queue)

DISCOVERED=()               # discovered variants, as "language/variant"
SKIPPED=()                  # variants skipped at discovery, as "language/variant|reason"
declare -A DIM=()           # DIM["language/variant|key"]=value, from dimensions.tsv
DIM_KEYS=()                 # union of all dimension keys (runner keys excluded), sorted

# Prints a message to stderr.
log() { echo "$(basename "$0"): $*" >&2; }

# Prints a warning to stderr.
warn() { echo "$(basename "$0"): WARNING: $*" >&2; }

# Prints an error to stderr and stops.
die() { echo "$(basename "$0"): error: $*" >&2; exit 1; }

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

# Fills DISCOVERED with every impl/<language>/<variant>/ folder; folders starting with _
# are shared code. A variant missing build.sh, run.sh or dimensions.tsv is skipped with a
# warning (recorded in failures.tsv later, like a failed build). Warns if a language has no
# usable `sequential` variant.
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
            DISCOVERED+=("$lang/$variant")
        done
    done
    for lang_dir in "$ROOT"/impl/*/; do
        lang="$(basename "$lang_dir")"
        [[ "$lang" != _* ]] || continue
        in_list "$lang/sequential" "${DISCOVERED[@]}" \
            || warn "language '$lang' has no usable 'sequential' variant (the baseline of every speedup)"
    done
    [[ ${#DISCOVERED[@]} -gt 0 ]] || die "no usable variants found under $ROOT/impl/<language>/<variant>/"
    log "discovered ${#DISCOVERED[@]} variants: ${DISCOVERED[*]}"
}

# Reads every dimensions.tsv into DIM and builds DIM_KEYS, the sorted union of the
# dimension keys. Each line is "key<TAB>value"; blank lines and lines starting with # are
# ignored. Runner keys (uses, modes, max_threads) are stored but are not result columns;
# a dimension key equal to a result column name is an error.
read_dimensions() {
    local id key value line_no
    local -a all_keys=() reserved=("${HEAD_COLS[@]}" "${PARAM_COLS[@]}" "${TAIL_COLS[@]}" "${MEASURE_COLS[@]}" "${LAST_COLS[@]}")
    for id in "${DISCOVERED[@]}"; do
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
