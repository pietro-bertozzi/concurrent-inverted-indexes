// The benchmark harness shared by every C variant: parses the command line, runs the
// repetitions (warm-ups first) and prints one TSV row per repetition on stdout, in the
// measurement columns of docs/design.md, section Results. Logs go to stderr.
//
// Every repetition starts from scratch: the input is read again, the index is new, the
// generators restart from --seed, so all repetitions do identical work.
//
// Every phase runs on N worker threads (also with N = 1): documents are handed out from a
// shared counter (workers_index), and each worker follows its own operation plan (OpPlan).
#pragma once
#include "args.h"
#include "index.h"

// Entry point of a variant: max_threads is the highest worker count it supports, factory makes
// an empty index from the command line (the variant reads the numeric parameters it uses), uses lists the numeric parameters (partitions, batch, queue) the variant
// accepts. Returns the exit status: 0, or 1 after an error message on stderr (fatal errors
// end the process with 1 directly).
int run_variant(int argc, char **argv, int max_threads, Index *(*factory)(const Args *),
                const char *const *uses, int uses_count);
