package main

import (
	"strings"
	"sync"
	"sync/atomic"
)

// GlobalLockIndex (docs/experiment-plan.md, How each variant works): one map and one lock for
// the whole index. A worker tokenizes its document without the lock, then takes the lock and
// inserts up to batch postings per acquisition. A query holds the same lock while it reads
// and merges its lists.
type GlobalLockIndex struct {
	m        map[string]*PostingList
	mu       sync.Mutex // the standard, non-fair mutex
	batch    int
	postings int64 // guarded by mu
	docs     atomic.Int64
}

// NewGlobalLockIndex returns an empty index that inserts batch postings per lock acquisition.
func NewGlobalLockIndex(batch int) *GlobalLockIndex {
	return &GlobalLockIndex{m: make(map[string]*PostingList), batch: batch}
}

// globalWorker is one worker's private state: its tokenizing buffer and its query scratch space.
type globalWorker struct {
	x      *GlobalLockIndex
	doc    *DocPostings
	engine *QueryEngine
}

func (x *GlobalLockIndex) Worker() Worker {
	return &globalWorker{x: x, doc: NewDocPostings(), engine: NewQueryEngine()}
}

func (w *globalWorker) Add(docID int, text string) {
	x := w.x
	w.doc.Read(text) // no lock: private work
	n := len(w.doc.Terms)
	for from := 0; from < n; from += x.batch {
		to := from + x.batch
		if to > n || to < from { // to < from: overflow of a huge batch
			to = n
		}
		x.mu.Lock()
		for k := from; k < to; k++ {
			x.insert(w.doc.Terms[k], docID, int(w.doc.Freqs[k]))
		}
		x.postings += int64(to - from)
		x.mu.Unlock()
	}
	x.docs.Add(1)
}

func (w *globalWorker) Query(q *Queries, i int) int {
	w.x.mu.Lock()
	defer w.x.mu.Unlock()
	return w.engine.Run(w.x, q, i)
}

// insert adds one posting to the list of term; the caller holds the lock.
func (x *GlobalLockIndex) insert(term string, docID, freq int) {
	list := x.m[term]
	if list == nil {
		list = &PostingList{}
		// The term is a substring of the document line: clone it, so that the index owns its
		// keys and never keeps a document line alive.
		x.m[strings.Clone(term)] = list
	}
	list.Insert(docID, freq)
}

func (x *GlobalLockIndex) Get(term string) *PostingList { return x.m[term] }

func (x *GlobalLockIndex) Finish() { /* lists are kept sorted on every insert */ }

func (x *GlobalLockIndex) Docs() int { return int(x.docs.Load()) }

func (x *GlobalLockIndex) Terms() int { return len(x.m) }

func (x *GlobalLockIndex) Postings() int64 { return x.postings }

func (x *GlobalLockIndex) Each(visit func(term string, list *PostingList)) {
	for term, list := range x.m {
		visit(term, list)
	}
}
