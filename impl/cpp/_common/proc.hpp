// Process measurements from getrusage(RUSAGE_SELF): the same call every language uses
// (docs/design.md, section Process measurements). It covers all threads of the process.
#pragma once
#include <cstdint>

// The counters that grow during the run.
struct ProcSnapshot {
    int64_t userNs, sysNs, ctxVol, ctxInvol;

    // The counters of this reading minus those of an earlier one.
    ProcSnapshot minus(const ProcSnapshot& earlier) const {
        return {userNs - earlier.userNs, sysNs - earlier.sysNs,
                ctxVol - earlier.ctxVol, ctxInvol - earlier.ctxInvol};
    }
};

// Reads user and system CPU time (ns) and the context switches of the whole process.
ProcSnapshot procSnapshot();

// The peak resident memory of the process so far, in bytes (ru_maxrss, reported in kB).
int64_t peakRssBytes();
