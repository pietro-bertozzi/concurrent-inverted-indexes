package main

import (
	"runtime"
	"strings"
	"sync/atomic"
)

// PartitionWritersIndex (docs/experiment-plan.md, How each variant works): the index is P
// sub-maps by term, and every partition has exactly one owner, the only goroutine that ever
// writes it. The owners are the workers themselves: partition p belongs to worker p mod N. A
// worker groups its document's postings by partition into one private buffer per partition; a
// buffer that holds batch postings becomes a message. A message for a partition the worker owns
// is applied at once; any other goes into the owner's bounded queue (queue messages). A worker
// empties its own queue after every document and whenever it has to wait.
type PartitionWritersIndex struct {
	parts    []partition
	batch    int
	workers  int
	queues   []chan message // queues[w]: the mailbox of worker w
	finished atomic.Int64   // flush calls that have sent everything, over all phases
	docs     atomic.Int64
}

// partition is one sub-map with its posting count. Only its owner writes them.
type partition struct {
	m        map[string]*PostingList
	postings int64
}

// message is a full buffer of postings, all for one partition.
type message struct {
	part int
	buf  PostingBuffer
}

// NewPartitionWritersIndex returns an empty index of the given number of partitions, with one
// queue of the given capacity for each of the workers.
func NewPartitionWritersIndex(partitions, batch, queue, workers int) *PartitionWritersIndex {
	x := &PartitionWritersIndex{parts: make([]partition, partitions), batch: batch, workers: workers,
		queues: make([]chan message, workers)}
	for p := range x.parts {
		x.parts[p].m = make(map[string]*PostingList)
	}
	for w := range x.queues {
		x.queues[w] = make(chan message, queue)
	}
	return x
}

// writerWorker is one worker's private state: tokenizing buffer, grouping arrays, one posting
// buffer per partition and query scratch space.
type writerWorker struct {
	x      *PartitionWritersIndex
	doc    *DocPostings
	engine *QueryEngine
	number int             // this worker
	start  int             // the partition visited first (worker number mod P)
	phases int64           // flush calls made so far
	bufs   []PostingBuffer // postings waiting for each partition
	first  []int           // first[p] .. first[p+1]: slots of partition p in order
	next   []int
	partOf []int // partition of each posting of the document
	order  []int // posting numbers, grouped by partition
}

func (x *PartitionWritersIndex) Worker(number int) Worker {
	n := len(x.parts)
	w := &writerWorker{x: x, doc: NewDocPostings(), engine: NewQueryEngine(), number: number,
		start: number % n, bufs: make([]PostingBuffer, n), first: make([]int, n+1), next: make([]int, n)}
	for p := range w.bufs {
		w.bufs[p] = w.newBuffer()
	}
	return w
}

// newBuffer makes an empty buffer with room for min(batch, 1024) postings.
func (w *writerWorker) newBuffer() PostingBuffer {
	c := min(w.x.batch, 1024)
	return PostingBuffer{Terms: make([]string, 0, c), Docs: make([]int32, 0, c), Freqs: make([]int32, 0, c)}
}

func (w *writerWorker) Add(docID int, text string) {
	x := w.x
	w.doc.Read(text) // private work
	w.group()
	for i := range x.parts { // partitions from start, wrapping around
		p := (w.start + i) % len(x.parts)
		for s := w.first[p]; s < w.first[p+1]; s++ {
			k := w.order[s]
			w.bufs[p].Add(w.doc.Terms[k], docID, w.doc.Freqs[k])
			if w.bufs[p].Size() == x.batch { // full: one message of batch postings
				w.send(p)
			}
		}
	}
	x.docs.Add(1)
	w.drain() // regularly empty our own mailbox
}

// Flush ends the phase. First send everything still buffered. Then wait until every worker has
// done the same, emptying our own queue meanwhile: once all have sent, nothing new can arrive,
// and one last drain applies what is left. Every worker calls Flush once per phase.
func (w *writerWorker) Flush() {
	for i := range w.bufs {
		p := (w.start + i) % len(w.bufs)
		if w.bufs[p].Size() > 0 {
			w.send(p)
		}
	}
	w.phases++
	w.x.finished.Add(1)
	for w.x.finished.Load() < w.phases*int64(w.x.workers) {
		if !w.drain() {
			runtime.Gosched() // nothing to apply: let others run
		}
	}
	w.drain() // every message was queued before its sender's increment
}

// send hands the buffer of partition p to its owner: applied directly if that is us, else queued.
func (w *writerWorker) send(p int) {
	owner := p % w.x.workers
	if owner == w.number {
		w.apply(p, &w.bufs[p])
		w.bufs[p].Clear()
		return
	}
	m := message{part: p, buf: w.bufs[p]} // the full buffer travels; no copy of the postings
	w.bufs[p] = w.newBuffer()
	for {
		select {
		case w.x.queues[owner] <- m:
			return
		default: // queue full: backpressure
			if !w.drain() { // deadlock rule: empty our own queue before waiting
				runtime.Gosched()
			}
		}
	}
}

// drain applies every message in our own queue; it returns true if there was any.
func (w *writerWorker) drain() bool {
	any := false
	for {
		select {
		case m := <-w.x.queues[w.number]:
			w.apply(m.part, &m.buf)
			any = true
		default:
			return any
		}
	}
}

// apply inserts the postings of buf into partition p; only the owner of p calls it.
func (w *writerWorker) apply(p int, buf *PostingBuffer) {
	part := &w.x.parts[p]
	for k := range buf.Terms {
		list := part.m[buf.Terms[k]]
		if list == nil {
			list = &PostingList{}
			// The term is a substring of the document line: clone it, so that the index owns
			// its keys and never keeps a document line alive.
			part.m[strings.Clone(buf.Terms[k])] = list
		}
		list.Insert(int(buf.Docs[k]), int(buf.Freqs[k]))
	}
	part.postings += int64(buf.Size())
}

// group is a counting sort of the document's postings by partition: it fills first and order.
func (w *writerWorker) group() {
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

func (w *writerWorker) Query(q *Queries, i int) int {
	return w.engine.Run(w.x, q, i) // nobody writes in query mode: no lock
}

func (x *PartitionWritersIndex) Get(term string) *PostingList {
	return x.parts[PartitionOf(term, len(x.parts))].m[term]
}

func (x *PartitionWritersIndex) Finish() { /* lists are kept sorted on every insert */ }

func (x *PartitionWritersIndex) Docs() int { return int(x.docs.Load()) }

func (x *PartitionWritersIndex) Terms() int {
	n := 0
	for p := range x.parts {
		n += len(x.parts[p].m)
	}
	return n
}

func (x *PartitionWritersIndex) Postings() int64 {
	var n int64
	for p := range x.parts {
		n += x.parts[p].postings
	}
	return n
}

func (x *PartitionWritersIndex) Each(visit func(term string, list *PostingList)) {
	for p := range x.parts {
		for term, list := range x.parts[p].m {
			visit(term, list)
		}
	}
}
