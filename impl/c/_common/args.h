// The command line of docs/design.md, section CLI contract: long options only, each with one
// value; an unknown option, a missing value, a missing required option or an option that does
// not belong to the mode is an error (args_parse ends the process with the reason).
#pragma once
#include <stdint.h>

// The options the CLI knows, in the order of their slots in Args.values.
enum { OPT_MODE, OPT_INPUT, OPT_THREADS, OPT_REPEAT, OPT_WARMUP, OPT_QUERIES, OPT_OPS, OPT_PRELOAD,
       OPT_SEED, OPT_PARTITIONS, OPT_BATCH, OPT_QUEUE, OPT_LATENCIES, OPT_DUMP, OPT_COUNT };

typedef struct {
    const char *mode;        // build, query, mixed-95-5, mixed-50-50, mixed-20-80
    const char *input;
    int threads, repeat, warmup;
    const char *queries;     // query and mixed modes
    int ops;                 // query and mixed modes
    int preload;             // mixed modes
    uint64_t seed;           // query and mixed modes
    const char *latencies;   // query and mixed modes
    const char *dump;        // optional, NULL if absent
    const char *values[OPT_COUNT];   // raw text of every option given, NULL if absent
} Args;

// Parses and validates argv (without the program name); ends the process on any error.
Args args_parse(int argc, char **argv);

// True if the command line carries the numeric parameter name (partitions, batch, queue).
int args_has(const Args *a, const char *name);

// The insert share in percent of mode: 100 for build, 0 for query, 5/50/80 for mixed; -1 if not a mode.
int args_insert_share(const char *mode);
