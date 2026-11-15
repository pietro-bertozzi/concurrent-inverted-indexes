package main

import (
	"sync"
	"sync/atomic"
)

// The worker loop shared by every variant (docs/experiment-plan.md, Rules every variant
// follows): N goroutines run the same body, and documents are handed out dynamically from a
// shared counter. A worker that finishes a document takes the next one, so faster cores
// simply take more documents.

// RunWorkers runs body(w) for w = 0 .. n-1, each in its own goroutine, and waits for all of them.
func RunWorkers(n int, body func(w int)) {
	var wg sync.WaitGroup
	wg.Add(n)
	for w := 0; w < n; w++ {
		go func(w int) {
			defer wg.Done()
			body(w)
		}(w)
	}
	wg.Wait()
}

// IndexDocs indexes the documents from (inclusive) to to (exclusive): each worker repeatedly
// takes the next document ID from a shared atomic counter (d = counter++) and adds it. The
// counter is touched once per document, not once per word, so it costs almost nothing.
func IndexDocs(workers []Worker, docs []string, from, to int) {
	var next atomic.Int64
	next.Store(int64(from))
	RunWorkers(len(workers), func(w int) {
		me := workers[w]
		for {
			d := int(next.Add(1) - 1)
			if d >= to {
				me.Flush() // the end of the phase: nothing may stay in a private buffer
				return
			}
			me.Add(d, docs[d])
		}
	})
}
