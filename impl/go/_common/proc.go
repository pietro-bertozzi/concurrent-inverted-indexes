package main

import (
	"fmt"
	"syscall"
)

// ProcSnapshot is a reading of the counters that grow during the run.
type ProcSnapshot struct {
	UserNs, SysNs, CtxVol, CtxInvol int64
}

// Minus returns the counters of s minus those of an earlier reading.
func (s ProcSnapshot) Minus(earlier ProcSnapshot) ProcSnapshot {
	return ProcSnapshot{s.UserNs - earlier.UserNs, s.SysNs - earlier.SysNs,
		s.CtxVol - earlier.CtxVol, s.CtxInvol - earlier.CtxInvol}
}

// rusage calls getrusage(RUSAGE_SELF): the same call every language uses (docs/design.md,
// section Process measurements). It covers all threads of the process.
func rusage() (syscall.Rusage, error) {
	var ru syscall.Rusage
	if err := syscall.Getrusage(syscall.RUSAGE_SELF, &ru); err != nil {
		return ru, fmt.Errorf("getrusage failed: %w", err)
	}
	return ru, nil
}

// Snapshot reads user and system CPU time (ns) and the context switches of the whole process.
func Snapshot() (ProcSnapshot, error) {
	ru, err := rusage()
	if err != nil {
		return ProcSnapshot{}, err
	}
	return ProcSnapshot{
		UserNs:   int64(ru.Utime.Sec)*1_000_000_000 + int64(ru.Utime.Usec)*1_000,
		SysNs:    int64(ru.Stime.Sec)*1_000_000_000 + int64(ru.Stime.Usec)*1_000,
		CtxVol:   int64(ru.Nvcsw),
		CtxInvol: int64(ru.Nivcsw),
	}, nil
}

// PeakRssBytes is the peak resident memory of the process so far (ru_maxrss, in kB on Linux).
func PeakRssBytes() (int64, error) {
	ru, err := rusage()
	if err != nil {
		return 0, err
	}
	return int64(ru.Maxrss) * 1024, nil
}
