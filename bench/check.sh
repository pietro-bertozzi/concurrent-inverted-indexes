#!/usr/bin/env bash
# Correctness check (docs/design.md, section Correctness).
#
#   ./bench/check.sh [params-file]     (default: bench/params-quick.sh)
#
# Builds every variant, runs it with --dump on the hand-made test data (data/tiny/) in
# build and query mode (those it declares in `modes`), with every worker count of the params
# file that its max_threads allows (run shape and numeric parameters: the CHECK_* variables of the
# params file), and compares the dump with data/tiny/expected-dump.tsv
# using cmp. A variant passes if all its runs produce a byte-identical dump.
#
# stdout: one TSV row per variant (variant, result, detail), with a header.
# stderr: progress. Exit code: 0 if every variant passed, 1 otherwise.
set -euo pipefail

# shellcheck source=bench/common.sh
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

TINY="$DATA/tiny"
WORK=""                     # temporary folder for logs and dumps

# Removes the temporary folder.
cleanup() { [[ -z "$WORK" ]] || rm -rf "$WORK"; }

# Prints the arguments of a run of variant $1 in mode $2 with $3 workers, one per line.
# Numeric parameters a variant asks for in `uses` take the CHECK_* values of the params file.
run_args() {
    local id="$1" mode="$2" threads="$3"
    echo --mode; echo "$mode"
    echo --input; echo "$TINY/corpus.txt"
    echo --threads; echo "$threads"
    # The dump comes from a separate, untimed run: CHECK_REPEAT and CHECK_WARMUP are small.
    echo --repeat; echo "$CHECK_REPEAT"
    echo --warmup; echo "$CHECK_WARMUP"
    echo --dump; echo "$WORK/dump.tsv"
    if [[ "$mode" == query ]]; then
        echo --queries; echo "$TINY/queries.txt"
        echo --ops; wc -l < "$TINY/queries.txt"
        echo --seed; echo "$SEED"
        echo --latencies; echo "$WORK/latencies.tsv"
    fi
    if in_csv partitions "$(dim "$id" uses)"; then echo --partitions; echo "$CHECK_PARTITIONS"; fi
    if in_csv batch "$(dim "$id" uses)"; then echo --batch; echo "$CHECK_BATCH"; fi
    if in_csv queue "$(dim "$id" uses)"; then echo --queue; echo "$CHECK_QUEUE"; fi
}

# Runs variant $1 in mode $2 with $3 workers and compares the dump with the expected one.
# Succeeds if they are identical; otherwise prints the reason to stdout and fails.
check_run() {
    local id="$1" mode="$2" threads="$3" status=0 line
    local -a args
    mapfile -t args < <(run_args "$id" "$mode" "$threads")
    rm -f "$WORK/dump.tsv" "$WORK/latencies.tsv"
    timeout --kill-after=30s "$TIMEOUT" "$ROOT/impl/$id/run.sh" "${args[@]}" \
        > "$WORK/stdout.txt" 2> "$WORK/stderr.txt" || status=$?
    if [[ "$status" -eq 124 || "$status" -eq 137 ]]; then
        echo "$mode threads=$threads: timeout"; return 1
    elif [[ "$status" -ne 0 ]]; then
        echo "$mode threads=$threads: exit status $status"; return 1
    elif [[ ! -f "$WORK/dump.tsv" ]]; then
        echo "$mode threads=$threads: no dump written"; return 1
    elif ! line="$(cmp "$WORK/dump.tsv" "$TINY/expected-dump.tsv" 2>&1)"; then
        echo "$mode threads=$threads: dump differs ($line)"; return 1
    fi
}

# Builds variant $1 and checks it in every mode and worker count; prints its TSV row.
# Returns 1 if it failed.
check_variant() {
    local id="$1" mode threads max count=0 detail
    local -a modes workers=()
    if ! (cd "$ROOT/impl/$id" && ./build.sh) > "$WORK/build.log" 2>&1; then
        join_tabs "$id" FAIL "build failed: $(tail -n 3 "$WORK/build.log" | tr '\n\t' '  ')"
        return 1
    fi
    max="${DIM[$id|max_threads]:-0}"
    for threads in $THREADS; do
        if (( max == 0 || threads <= max )); then workers+=("$threads"); fi
    done
    IFS=, read -ra modes <<<"${DIM[$id|modes]}"
    for mode in "${modes[@]}"; do
        [[ "$mode" == build || "$mode" == query ]] || continue
        for threads in "${workers[@]}"; do
            if ! detail="$(check_run "$id" "$mode" "$threads")"; then
                join_tabs "$id" FAIL "$detail"
                return 1
            fi
            count=$(( count + 1 ))
        done
    done
    (( count > 0 )) || { join_tabs "$id" FAIL "nothing checked: no build or query mode, or no worker count within max_threads"; return 1; }
    join_tabs "$id" PASS "$count runs identical to the expected dump"
}

# Loads the params file, discovers and checks every variant, prints the report.
main() {
    [[ $# -le 1 ]] || die "usage: bench/check.sh [params-file]"
    local params="${1:-$ROOT/bench/params-quick.sh}" id row failed=0 passed=0 file
    [[ -f "$params" ]] || die "params file '$params' does not exist"
    for file in corpus.txt queries.txt expected-dump.tsv; do
        [[ -f "$TINY/$file" ]] || die "test data $TINY/$file is missing"
    done
    for file in timeout cmp awk; do
        command -v "$file" >/dev/null || die "required tool '$file' not found in PATH"
    done
    # shellcheck source=/dev/null
    source "$params"
    for file in THREADS SEED TIMEOUT CHECK_REPEAT CHECK_WARMUP CHECK_PARTITIONS CHECK_BATCH CHECK_QUEUE; do
        [[ -n "${!file+set}" ]] || die "$params does not define $file"
    done
    for file in CHECK_REPEAT CHECK_WARMUP CHECK_PARTITIONS CHECK_BATCH CHECK_QUEUE; do
        [[ "${!file}" =~ ^[0-9]+$ ]] || die "$file must be a non-negative integer, got '${!file}'"
    done
    (( CHECK_REPEAT >= 1 )) || die "CHECK_REPEAT must be at least 1"
    discover_variants
    read_dimensions
    WORK="$(mktemp -d)"
    trap cleanup EXIT
    join_tabs variant result detail
    for id in "${VARIANTS[@]}"; do
        log "checking $id"
        if row="$(check_variant "$id")"; then passed=$(( passed + 1 )); else failed=$(( failed + 1 )); fi
        echo "$row"
        log "${row#*$'\t'}"
    done
    log "$passed passed, $failed failed"
    (( failed == 0 ))
}

main "$@"
