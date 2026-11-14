#!/usr/bin/env bash
# Correctness check (docs/design.md, section Correctness).
#
#   ./bench/check.sh [params-file]     (default: bench/params-quick.sh)
#
# Builds every variant, runs it with --dump on the hand-made test data (data/tiny/) in
# every mode it declares in `modes`, with every worker count of the params file that its
# max_threads allows (run shape and numeric parameters: the CHECK_* variables of the params
# file), and compares the dump with cmp. Build and query modes are compared with
# data/tiny/expected-dump.tsv; mixed modes with a reference dump: the variant's own build
# mode, one worker, on the first preload + inserts documents of the tiny corpus. A variant
# passes if all its runs produce a byte-identical dump.
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

# Prints the insert share of mode $1 in percent: 0 for build and query, the last number of
# the name for mixed modes (mixed-95-5 gives 5).
insert_share() {
    case "$1" in
        build) echo 100 ;;   # build inserts every document; callers never use it as a share
        query) echo 0 ;;
        mixed-*) echo "${1##*-}" ;;
        *) die "unknown mode '$1'" ;;
    esac
}

# Prints the number of inserts of a mixed run in mode $1: floor(CHECK_OPS * share / 100).
insert_count() { echo $(( CHECK_OPS * $(insert_share "$1") / 100 )); }

# Prints the arguments of a run of variant $1 in mode $2 with $3 workers on corpus $4,
# dumping to $5, one per line.
# Numeric parameters a variant asks for in `uses` take the CHECK_* values of the params file.
run_args() {
    local id="$1" mode="$2" threads="$3" input="$4" dump="$5"
    echo --mode; echo "$mode"
    echo --input; echo "$input"
    echo --threads; echo "$threads"
    # The dump comes from a separate, untimed run: CHECK_REPEAT and CHECK_WARMUP are small.
    echo --repeat; echo "$CHECK_REPEAT"
    echo --warmup; echo "$CHECK_WARMUP"
    echo --dump; echo "$dump"
    if [[ "$mode" != build ]]; then
        echo --queries; echo "$TINY/queries.txt"
        echo --ops; echo "$CHECK_OPS"
        echo --seed; echo "$SEED"
        echo --latencies; echo "$WORK/latencies.tsv"
    fi
    if [[ "$mode" == mixed-* ]]; then echo --preload; echo "$CHECK_PRELOAD"; fi
    if in_csv partitions "$(dim "$id" uses)"; then echo --partitions; echo "$CHECK_PARTITIONS"; fi
    if in_csv batch "$(dim "$id" uses)"; then echo --batch; echo "$CHECK_BATCH"; fi
    if in_csv queue "$(dim "$id" uses)"; then echo --queue; echo "$CHECK_QUEUE"; fi
}

# Runs variant $1 in mode $2 with $3 workers on corpus $4, writing the dump to $5. Succeeds
# if it exits 0 and writes the dump; otherwise prints the reason to stdout and fails.
run_variant() {
    local id="$1" mode="$2" threads="$3" input="$4" dump="$5" status=0
    local -a args
    mapfile -t args < <(run_args "$id" "$mode" "$threads" "$input" "$dump")
    rm -f "$dump" "$WORK/latencies.tsv"
    timeout --kill-after=30s "$TIMEOUT" "$ROOT/impl/$id/run.sh" "${args[@]}" \
        > "$WORK/stdout.txt" 2> "$WORK/stderr.txt" || status=$?
    if [[ "$status" -eq 124 || "$status" -eq 137 ]]; then
        echo "$mode threads=$threads: timeout"; return 1
    elif [[ "$status" -ne 0 ]]; then
        echo "$mode threads=$threads: exit status $status"; return 1
    elif [[ ! -f "$dump" ]]; then
        echo "$mode threads=$threads: no dump written"; return 1
    fi
}

# Runs variant $1 in mode $2 with $3 workers and compares its dump with the file $4.
# Succeeds if they are identical; otherwise prints the reason to stdout and fails.
check_run() {
    local id="$1" mode="$2" threads="$3" expected="$4" line
    run_variant "$id" "$mode" "$threads" "$TINY/corpus.txt" "$WORK/dump.tsv" || return 1
    if ! line="$(cmp "$WORK/dump.tsv" "$expected" 2>&1)"; then
        echo "$mode threads=$threads: dump differs ($line)"; return 1
    fi
}

# Makes the reference dump of mixed mode $2 for variant $1 in $WORK/reference.tsv: the
# variant's build mode, one worker, on the first preload + inserts documents of the tiny
# corpus (exactly the documents a mixed run ends up with). Fails with the reason on stdout.
make_reference() {
    local id="$1" mode="$2" detail
    in_csv build "${DIM[$id|modes]}" || { echo "$mode: no build mode for the reference"; return 1; }
    head -n $(( CHECK_PRELOAD + $(insert_count "$mode") )) "$TINY/corpus.txt" > "$WORK/prefix.txt"
    if ! detail="$(run_variant "$id" build 1 "$WORK/prefix.txt" "$WORK/reference.tsv")"; then
        echo "$mode: reference run failed ($detail)"; return 1
    fi
}

# Builds variant $1 and checks it in every mode and worker count; prints its TSV row.
# Returns 1 if it failed.
check_variant() {
    local id="$1" mode threads max count=0 detail expected
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
        expected="$TINY/expected-dump.tsv"
        if [[ "$mode" == mixed-* ]]; then
            if ! detail="$(make_reference "$id" "$mode")"; then join_tabs "$id" FAIL "$detail"; return 1; fi
            expected="$WORK/reference.tsv"
        fi
        for threads in "${workers[@]}"; do
            if ! detail="$(check_run "$id" "$mode" "$threads" "$expected")"; then
                join_tabs "$id" FAIL "$detail"
                return 1
            fi
            count=$(( count + 1 ))
        done
    done
    (( count > 0 )) || { join_tabs "$id" FAIL "nothing checked: no mode declared, or no worker count within max_threads"; return 1; }
    join_tabs "$id" PASS "$count runs identical to the expected or reference dump"
}

# Stops with an error if a mixed run would need more documents than the tiny corpus has:
# CHECK_PRELOAD + the inserts of the largest insert share among the declared mixed modes.
check_fit() {
    local id mode need docs max=0
    local -a modes
    for id in "${VARIANTS[@]}"; do
        IFS=, read -ra modes <<<"${DIM[$id|modes]}"
        for mode in "${modes[@]}"; do
            [[ "$mode" == mixed-* ]] || continue
            need=$(( CHECK_PRELOAD + $(insert_count "$mode") ))
            (( need > max )) && max="$need"
        done
    done
    docs="$(wc -l < "$TINY/corpus.txt")"
    (( max <= docs )) || die "mixed checks need $max documents (CHECK_PRELOAD=$CHECK_PRELOAD + inserts of CHECK_OPS=$CHECK_OPS), but $TINY/corpus.txt has only $docs: lower CHECK_OPS or CHECK_PRELOAD"
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
    for file in THREADS SEED TIMEOUT CHECK_REPEAT CHECK_WARMUP CHECK_PARTITIONS CHECK_BATCH CHECK_QUEUE CHECK_OPS CHECK_PRELOAD; do
        [[ -n "${!file+set}" ]] || die "$params does not define $file"
    done
    for file in CHECK_REPEAT CHECK_WARMUP CHECK_PARTITIONS CHECK_BATCH CHECK_QUEUE CHECK_OPS CHECK_PRELOAD; do
        [[ "${!file}" =~ ^[0-9]+$ ]] || die "$file must be a non-negative integer, got '${!file}'"
    done
    (( CHECK_REPEAT >= 1 )) || die "CHECK_REPEAT must be at least 1"
    (( CHECK_OPS >= 1 )) || die "CHECK_OPS must be at least 1"
    discover_variants
    read_dimensions
    check_fit
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
