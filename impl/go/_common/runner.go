package main

import (
	"bufio"
	"fmt"
	"io"
	"os"
	"runtime"
	"strconv"
	"strings"
	"sync/atomic"
	"time"
)

// The benchmark harness shared by every Go variant: parses the command line, runs the
// repetitions (warm-ups first) and prints one TSV row per repetition on stdout, in the
// measurement columns of docs/design.md, section Results. Logs go to stderr.
//
// Every repetition starts from scratch: the input is read again, the index is new, the
// generators restart from --seed, so all repetitions do identical work.
//
// Every phase runs on N worker goroutines (also with N = 1): documents are handed out from a
// shared counter (IndexDocs), and each worker follows its own operation plan (OpPlan).

// Run is the entry point of a variant: maxThreads is the highest worker count it supports,
// factory makes an empty index from the command line (the variant reads the numeric
// parameters it uses), uses lists those parameters (partitions, batch, queue). It exits with
// status 1 on error.
func Run(argv []string, maxThreads int, factory func(a *Args) Index, uses ...string) {
	a, err := ParseArgs(argv)
	if err == nil {
		err = execute(a, maxThreads, factory, uses)
	}
	if err != nil {
		fmt.Fprintln(os.Stderr, "error: "+err.Error())
		os.Exit(1)
	}
}

// execute runs all repetitions of a.
// clockCostNs returns the mean cost in ns of one time.Now() call: 1,000,000 back-to-back
// readings (docs/design.md, Query latencies).
func clockCostNs() int64 {
	const n = 1000000
	var sink int64
	start := time.Now()
	for i := 0; i < n; i++ {
		sink += time.Now().UnixNano()
	}
	total := time.Since(start).Nanoseconds()
	if sink == 42 { // the readings are consumed, so they cannot be removed
		fmt.Fprint(os.Stderr, "")
	}
	return total / n
}

func execute(a *Args, maxThreads int, factory func(a *Args) Index, uses []string) error {
	if a.Threads > maxThreads {
		return fmt.Errorf("this variant supports at most %d worker(s), got --threads %d", maxThreads, a.Threads)
	}
	for _, name := range []string{"partitions", "batch", "queue"} {
		used := contains(uses, name)
		if a.Has(name) && !used {
			return fmt.Errorf("option --%s is not used by this variant", name)
		}
		if !a.Has(name) && used {
			return fmt.Errorf("missing required option --%s for this variant", name)
		}
	}
	build := a.Mode == "build"
	var queries *Queries
	var latencies *bufio.Writer
	if !build {
		var err error
		if queries, err = LoadQueries(a.Queries); err != nil {
			return err
		}
	}
	fmt.Fprintf(os.Stderr, "go %s, GOMAXPROCS %d, mode %s, %d worker(s), %d warm-up + %d repetitions\n",
		strings.TrimPrefix(runtime.Version(), "go"), runtime.GOMAXPROCS(0), a.Mode, a.Threads, a.Warmup, a.Repeat)
	fmt.Fprintf(os.Stderr, "clock reading cost: %d ns (mean of 1000000 back-to-back readings)\n", clockCostNs())
	if !build {
		f, err := os.Create(a.Latencies)
		if err != nil {
			return err
		}
		defer f.Close()
		latencies = bufio.NewWriter(f)
	}
	total := a.Warmup + a.Repeat
	for rep := 0; rep < total; rep++ {
		if err := repetition(a, rep, rep < a.Warmup, rep == total-1, queries, factory, latencies); err != nil {
			return err
		}
	}
	if latencies != nil {
		return latencies.Flush()
	}
	return nil
}

// repetition is one repetition: load, index (build / preload), run, finish; prints its row.
func repetition(a *Args, rep int, warmup, last bool, queries *Queries, factory func(a *Args) Index, latencies *bufio.Writer) error {
	build := a.Mode == "build"
	mixed := strings.HasPrefix(a.Mode, "mixed-")
	share := InsertShare(a.Mode)
	before, err := Snapshot()
	if err != nil {
		return err
	}

	t := time.Now()
	docs, err := load(a.Input)
	if err != nil {
		return err
	}
	loadNs := time.Since(t).Nanoseconds()
	if len(docs) == 0 {
		return fmt.Errorf("%s has no documents", a.Input)
	}

	// The plans (the operation order of every worker) are made before timing starts.
	n := a.Threads
	var plans []*OpPlan
	opsQuery, opsInsert := 0, 0
	if !build {
		inserts := TotalInserts(a.Ops, share)
		if a.Preload+inserts > len(docs) {
			return fmt.Errorf("mixed run needs %d documents (preload %d + %d inserts) but %s has %d",
				a.Preload+inserts, a.Preload, inserts, a.Input, len(docs))
		}
		plans = make([]*OpPlan, n)
		for w := 0; w < n; w++ {
			plans[w] = MakeOpPlan(a.Ops, share, n, w, a.Seed, queries.Size())
			opsQuery += plans[w].Queries
			opsInsert += plans[w].Inserts
		}
	}

	index := factory(a)
	workers := make([]Worker, n) // one handle per worker, made before timing
	for w := range workers {
		workers[w] = index.Worker(w)
	}
	var preloadNs, buildNs, runNs, finalizeNs, resultTotal int64
	var latency [][]int64

	if build || a.Mode == "query" {
		t = time.Now()
		IndexDocs(workers, docs, 0, len(docs))
		buildNs = time.Since(t).Nanoseconds()
		if !build { // queries need the finished index
			t = time.Now()
			index.Finish()
			finalizeNs = time.Since(t).Nanoseconds()
		}
	} else {
		t = time.Now()
		IndexDocs(workers, docs, 0, a.Preload)
		preloadNs = time.Since(t).Nanoseconds()
	}
	if build {
		t = time.Now()
		index.Finish()
		finalizeNs = time.Since(t).Nanoseconds()
	}

	if !build {
		latency = make([][]int64, n)
		for w := range latency {
			latency[w] = make([]int64, plans[w].Queries)
		}
		results := make([]int64, n)
		var nextDoc atomic.Int64 // the shared insert counter
		nextDoc.Store(int64(a.Preload))
		t = time.Now()
		RunWorkers(n, func(w int) {
			results[w] = runPlan(workers[w], plans[w], queries, docs, &nextDoc, latency[w])
		})
		runNs = time.Since(t).Nanoseconds()
		for _, r := range results {
			resultTotal += r
		}
		if mixed {
			t = time.Now()
			index.Finish()
			finalizeNs = time.Since(t).Nanoseconds()
		}
		fmt.Fprintf(os.Stderr, "rep %d: sum of query result sizes = %d\n", rep, resultTotal)
	}

	used, err := Snapshot()
	if err != nil {
		return err
	}
	used = used.Minus(before)
	peak, err := PeakRssBytes()
	if err != nil {
		return err
	}
	w := 0
	if warmup {
		w = 1
	}
	row := strings.Join([]string{itoa(int64(rep)), itoa(int64(w)),
		itoa(loadNs), itoa(preloadNs), itoa(buildNs), itoa(runNs), itoa(finalizeNs),
		itoa(int64(opsQuery)), itoa(int64(opsInsert)),
		itoa(int64(index.Docs())), itoa(int64(index.Terms())), itoa(index.Postings()),
		itoa(peak), itoa(used.UserNs), itoa(used.SysNs), itoa(used.CtxVol), itoa(used.CtxInvol)}, "\t")

	// After the timers have stopped: the latencies, the row, the dump.
	for _, l := range latency {
		writeLatencies(latencies, rep, w, l)
	}
	fmt.Println(row)
	if a.Dump != "" && last {
		return WriteDump(index, a.Dump)
	}
	return nil
}

// runPlan is one worker's share of the timed phase: it follows its plan, timing every query
// with two clock readings around it. An insert takes the next document from the shared counter
// (whichever worker inserts next takes the next document). It returns the sum of the query
// result sizes.
func runPlan(me Worker, plan *OpPlan, queries *Queries, docs []string, nextDoc *atomic.Int64, latency []int64) int64 {
	var total int64
	cursor, li := plan.QueryOffset, 0
	for _, typ := range plan.Types {
		if typ == OpQuery {
			start := time.Now()
			total += int64(me.Query(queries, cursor))
			latency[li] = time.Since(start).Nanoseconds()
			li++
			cursor++
			if cursor == queries.Size() {
				cursor = 0
			}
		} else {
			d := int(nextDoc.Add(1) - 1)
			me.Add(d, docs[d])
		}
	}
	me.Flush() // the end of the phase: nothing may stay in a private buffer
	return total
}

// load reads the input file: one string per line (document ID = position).
func load(file string) ([]string, error) {
	f, err := os.Open(file)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	in := bufio.NewReaderSize(f, 1<<20)
	var docs []string
	for {
		line, err := in.ReadString('\n')
		if len(line) > 0 {
			docs = append(docs, strings.TrimRight(line, "\r\n"))
		}
		if err == io.EOF {
			return docs, nil
		}
		if err != nil {
			return nil, err
		}
	}
}

// writeLatencies appends the latencies of one repetition to the latency file: rep, warmup, latency_ns.
func writeLatencies(out *bufio.Writer, rep, warmup int, latency []int64) {
	prefix := strconv.Itoa(rep) + "\t" + strconv.Itoa(warmup) + "\t"
	for _, ns := range latency {
		out.WriteString(prefix)
		out.WriteString(strconv.FormatInt(ns, 10))
		out.WriteByte('\n')
	}
}

func itoa(n int64) string { return strconv.FormatInt(n, 10) }

func contains(list []string, s string) bool {
	for _, x := range list {
		if x == s {
			return true
		}
	}
	return false
}
