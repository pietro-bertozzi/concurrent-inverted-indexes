package main

import (
	"fmt"
	"strconv"
	"strings"
)

// Args is the command line of docs/design.md, section CLI contract: long options only, each
// with one value; an unknown option, a missing value, a missing required option or an option
// that does not belong to the mode is an error.
type Args struct {
	Mode                    string // build, query, mixed-95-5, mixed-50-50, mixed-20-80
	Input                   string
	Threads, Repeat, Warmup int
	Queries                 string // query and mixed modes
	Ops                     int    // query and mixed modes
	Preload                 int    // mixed modes
	Seed                    uint64 // query and mixed modes
	Latencies               string // query and mixed modes
	Dump                    string // optional, "" if absent
	// Numeric parameters; 0 if absent (a variant checks the ones it uses).
	Partitions, Batch, Queue int
	values                   map[string]string
}

var knownOptions = map[string]bool{"mode": true, "input": true, "threads": true, "repeat": true,
	"warmup": true, "queries": true, "ops": true, "preload": true, "seed": true,
	"partitions": true, "batch": true, "queue": true, "latencies": true, "dump": true}

// ParseArgs parses and validates argv (without the program name); the error says why not.
func ParseArgs(argv []string) (*Args, error) {
	a := &Args{values: map[string]string{}}
	for i := 0; i < len(argv); i += 2 {
		if !strings.HasPrefix(argv[i], "--") {
			return nil, fmt.Errorf("expected an option, got '%s'", argv[i])
		}
		name := argv[i][2:]
		if !knownOptions[name] {
			return nil, fmt.Errorf("unknown option %s", argv[i])
		}
		if i+1 >= len(argv) {
			return nil, fmt.Errorf("option %s has no value", argv[i])
		}
		if _, dup := a.values[name]; dup {
			return nil, fmt.Errorf("option %s given twice", argv[i])
		}
		a.values[name] = argv[i+1]
	}
	var err error
	if a.Mode, err = a.required("mode"); err != nil {
		return nil, err
	}
	query := a.Mode == "query"
	mixed := strings.HasPrefix(a.Mode, "mixed-")
	if InsertShare(a.Mode) < 0 {
		return nil, fmt.Errorf("unknown mode '%s'", a.Mode)
	}
	if a.Input, err = a.required("input"); err != nil {
		return nil, err
	}
	if a.Threads, err = a.integer("threads", 1); err != nil {
		return nil, err
	}
	if a.Repeat, err = a.integer("repeat", 1); err != nil {
		return nil, err
	}
	if a.Warmup, err = a.integer("warmup", 0); err != nil {
		return nil, err
	}
	// Options that only some modes use: required there, an error elsewhere.
	for _, name := range []string{"queries", "ops", "seed", "latencies"} {
		if query || mixed {
			_, err = a.required(name)
		} else {
			err = a.forbidden(name)
		}
		if err != nil {
			return nil, err
		}
	}
	if mixed {
		_, err = a.required("preload")
	} else {
		err = a.forbidden("preload")
	}
	if err != nil {
		return nil, err
	}
	a.Queries = a.values["queries"]
	a.Latencies = a.values["latencies"]
	a.Dump = a.values["dump"]
	if query || mixed {
		if a.Ops, err = a.integer("ops", 0); err != nil {
			return nil, err
		}
		if a.Seed, err = strconv.ParseUint(a.values["seed"], 10, 64); err != nil {
			return nil, fmt.Errorf("option --seed must be an unsigned integer, got '%s'", a.values["seed"])
		}
	}
	if mixed {
		if a.Preload, err = a.integer("preload", 0); err != nil {
			return nil, err
		}
	}
	for name, field := range map[string]*int{"partitions": &a.Partitions, "batch": &a.Batch, "queue": &a.Queue} {
		if a.Has(name) {
			if *field, err = a.integer(name, 1); err != nil {
				return nil, err
			}
		}
	}
	return a, nil
}

// InsertShare is the insert share in percent of mode: 100 for build, 0 for query, 5/50/80
// for the mixed modes; -1 if mode is not a mode.
func InsertShare(mode string) int {
	switch mode {
	case "build":
		return 100
	case "query":
		return 0
	}
	parts := strings.Split(mode, "-") // mixed-<query share>-<insert share>
	if len(parts) != 3 || parts[0] != "mixed" {
		return -1
	}
	for _, p := range parts[1:] {
		if p == "" || strings.Trim(p, "0123456789") != "" {
			return -1
		}
	}
	share, err := strconv.Atoi(parts[2])
	if err != nil || share > 100 {
		return -1
	}
	return share
}

// Has reports whether the command line carries the numeric parameter name (partitions, batch, queue).
func (a *Args) Has(name string) bool { _, ok := a.values[name]; return ok }

func (a *Args) required(name string) (string, error) {
	v, ok := a.values[name]
	if !ok {
		return "", fmt.Errorf("missing required option --%s for mode %s", name, a.values["mode"])
	}
	return v, nil
}

func (a *Args) forbidden(name string) error {
	if _, ok := a.values[name]; ok {
		return fmt.Errorf("option --%s does not apply to mode %s", name, a.values["mode"])
	}
	return nil
}

// integer reads the required option name as an int32-range integer >= min.
func (a *Args) integer(name string, min int) (int, error) {
	v, err := a.required(name)
	if err != nil {
		return 0, err
	}
	n, perr := strconv.ParseInt(v, 10, 32)
	if perr != nil || int(n) < min {
		return 0, fmt.Errorf("option --%s must be an integer >= %d, got '%s'", name, min, v)
	}
	return int(n), nil
}
