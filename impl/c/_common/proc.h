// Process measurements from getrusage(RUSAGE_SELF): the same call every language uses
// (docs/design.md, section Process measurements). It covers all threads of the process.
#pragma once
#include <stdint.h>

// The counters that grow during the run.
typedef struct {
    int64_t user_ns, sys_ns, ctx_vol, ctx_invol;
} ProcSnapshot;

// Reads user and system CPU time (ns) and the context switches of the whole process.
ProcSnapshot proc_snapshot(void);

// The counters of now minus those of an earlier reading.
ProcSnapshot proc_minus(ProcSnapshot now, ProcSnapshot earlier);

// The peak resident memory of the process so far, in bytes (ru_maxrss, reported in kB).
int64_t proc_peak_rss_bytes(void);
