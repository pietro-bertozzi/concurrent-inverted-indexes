#define _POSIX_C_SOURCE 200809L
#include "proc.h"

#include <errno.h>
#include <string.h>
#include <sys/resource.h>

#include "util.h"

// Calls getrusage(RUSAGE_SELF); ends the process with the reason on failure.
static struct rusage read_usage(void) {
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) != 0) fatal("getrusage failed: %s", strerror(errno));
    return ru;
}

ProcSnapshot proc_snapshot(void) {
    struct rusage ru = read_usage();
    ProcSnapshot s;
    s.user_ns = (int64_t)ru.ru_utime.tv_sec * 1000000000 + (int64_t)ru.ru_utime.tv_usec * 1000;
    s.sys_ns = (int64_t)ru.ru_stime.tv_sec * 1000000000 + (int64_t)ru.ru_stime.tv_usec * 1000;
    s.ctx_vol = ru.ru_nvcsw;
    s.ctx_invol = ru.ru_nivcsw;
    return s;
}

ProcSnapshot proc_minus(ProcSnapshot now, ProcSnapshot earlier) {
    ProcSnapshot d = {now.user_ns - earlier.user_ns, now.sys_ns - earlier.sys_ns,
                      now.ctx_vol - earlier.ctx_vol, now.ctx_invol - earlier.ctx_invol};
    return d;
}

int64_t proc_peak_rss_bytes(void) { return (int64_t)read_usage().ru_maxrss * 1024; }
