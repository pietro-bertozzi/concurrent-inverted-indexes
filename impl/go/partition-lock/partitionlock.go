package main

import (
	"strings"
	"sync"
	"sync/atomic"
)

// PartitionLockIndex (docs/experiment-plan.md, How each variant works): the index is P
// sub-maps, a term lives in partition PartitionOf(term, P), and each partition has its own
// lock. A worker groups its document's postings by partition into one private buffer per
// partition; when a buffer holds batch postings (from one or several documents) it takes that
// partition's lock and inserts them all, and it flushes what is left at the end of every phase, visiting the partitions from (worker number mod P), wrapping around, so that workers
// do not all queue at partition 0. A query holds the locks of the partitions of its terms, the lower partition number first.
type PartitionLockIndex struct {
	parts []partition
	batch int
	docs  atomic.Int64
}

// partition is one sub-map with its lock and its posting count (guarded by the lock).
type partition struct {
	m        map[string]*PostingList
	mu       sync.Mutex // the standard, non-fair mutex
	postings int64
}

// NewPartitionLockIndex returns an empty index of the given number of partitions.
func NewPartitionLockIndex(partitions, batch int) *PartitionLockIndex {
	x := &PartitionLockIndex{parts: make([]partition, partitions), batch: batch}
	for p := range x.parts {
		x.parts[p].m = make(map[string]*PostingList)
	}
	return x
}

// partWorker is one worker's private state: tokenizing buffer, grouping arrays, one posting
// buffer per partition and query scratch space.
type partWorker struct {
	x      *PartitionLockIndex
	doc    *DocPostings
	engine *QueryEngine
	start  int             // the partition this worker visits first (worker number mod P)
	bufs   []PostingBuffer // postings waiting for each partition
	first  []int           // first[p] .. first[p+1]: slots of partition p in order
	next   []int
	partOf []int // partition of each posting of the document
	order  []int // posting numbers, grouped by partition
}

func (x *PartitionLockIndex) Worker(number int) Worker {
	n := len(x.parts)
	return &partWorker{x: x, doc: NewDocPostings(), engine: NewQueryEngine(),
		start: number % n, bufs: make([]PostingBuffer, n), first: make([]int, n+1), next: make([]int, n)}
}

func (w *partWorker) Add(docID int, text string) {
	x := w.x
	w.doc.Read(text) // no lock: private work
	w.group()
	for i := range x.parts { // partitions from start, wrapping around
		p := (w.start + i) % len(x.parts)
		buf := &w.bufs[p]
		for s := w.first[p]; s < w.first[p+1]; s++ {
			k := w.order[s]
			buf.Add(w.doc.Terms[k], docID, w.doc.Freqs[k])
			if buf.Size() == x.batch { // full: one acquisition for batch postings
				w.flushPartition(p)
			}
		}
	}
	x.docs.Add(1)
}

func (w *partWorker) Flush() {
	for i := range w.bufs {
		p := (w.start + i) % len(w.bufs)
		if w.bufs[p].Size() > 0 {
			w.flushPartition(p)
		}
	}
}

// flushPartition inserts the buffer of partition p under one acquisition of that partition's lock.
func (w *partWorker) flushPartition(p int) {
	part := &w.x.parts[p]
	buf := &w.bufs[p]
	part.mu.Lock()
	for k := range buf.Terms {
		insert(part, buf.Terms[k], int(buf.Docs[k]), int(buf.Freqs[k]))
	}
	part.postings += int64(buf.Size())
	part.mu.Unlock()
	buf.Clear()
}

// group is a counting sort of the document's postings by partition: it fills first and order.
func (w *partWorker) group() {
	n := len(w.doc.Terms)
	if len(w.partOf) < n {
		w.partOf = make([]int, 2*n)
		w.order = make([]int, 2*n)
	}
	parts := len(w.x.parts)
	clear(w.first)
	for k := 0; k < n; k++ {
		w.partOf[k] = PartitionOf(w.doc.Terms[k], parts)
		w.first[w.partOf[k]+1]++
	}
	for p := 0; p < parts; p++ {
		w.first[p+1] += w.first[p] // first[p] = start of partition p
	}
	copy(w.next, w.first[:parts])
	for k := 0; k < n; k++ {
		w.order[w.next[w.partOf[k]]] = k
		w.next[w.partOf[k]]++
	}
}

func (w *partWorker) Query(q *Queries, i int) int {
	parts := w.x.parts
	a := PartitionOf(q.First[i], len(parts))
	b := a
	if q.Kind[i] != QuerySimple {
		b = PartitionOf(q.Second[i], len(parts))
	}
	low, high := min(a, b), max(a, b) // fixed lock order: lower number first
	parts[low].mu.Lock()
	if high != low {
		parts[high].mu.Lock()
	}
	n := w.engine.Run(w.x, q, i)
	if high != low {
		parts[high].mu.Unlock()
	}
	parts[low].mu.Unlock()
	return n
}

// insert adds one posting to the list of term in part; the caller holds its lock.
func insert(part *partition, term string, docID, freq int) {
	list := part.m[term]
	if list == nil {
		list = &PostingList{}
		// The term is a substring of the document line: clone it, so that the index owns its
		// keys and never keeps a document line alive.
		part.m[strings.Clone(term)] = list
	}
	list.Insert(docID, freq)
}

func (x *PartitionLockIndex) Get(term string) *PostingList {
	return x.parts[PartitionOf(term, len(x.parts))].m[term]
}

func (x *PartitionLockIndex) Finish() { /* lists are kept sorted on every insert */ }

func (x *PartitionLockIndex) Docs() int { return int(x.docs.Load()) }

func (x *PartitionLockIndex) Terms() int {
	n := 0
	for p := range x.parts {
		n += len(x.parts[p].m)
	}
	return n
}

func (x *PartitionLockIndex) Postings() int64 {
	var n int64
	for p := range x.parts {
		n += x.parts[p].postings
	}
	return n
}

func (x *PartitionLockIndex) Each(visit func(term string, list *PostingList)) {
	for p := range x.parts {
		for term, list := range x.parts[p].m {
			visit(term, list)
		}
	}
}
