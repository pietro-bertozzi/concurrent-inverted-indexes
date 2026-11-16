package main

import "strings"

// PrivateMergeIndex (docs/experiment-plan.md, How each variant works). While building, every
// worker writes only into its own private index, with no lock: its terms are already divided
// into N piles by the partition hash (P = N, the number of workers). In Finish worker i
// merges pile i of every worker, so all workers merge at once, each on different terms, with
// no lock either. A worker takes increasing document IDs, so each private list is already
// sorted and the merge of a term is an N-way merge in document order. The result is an index
// partitioned by term; queries (query mode, after Finish) read it with no lock.
//
// The N-way merge of one term has two versions that differ in nothing else (private-merge and
// private-merge-heap): a linear scan of the heads of the k lists for every posting (O(k) per
// posting) or a binary min-heap of the heads (O(log k) per posting).
type PrivateMergeIndex struct {
	n        int                       // workers = piles
	heap     bool                      // merge with a heap instead of a linear scan
	handles  []*pmWorker               // one per worker
	merged   []map[string]*PostingList // merged[i]: the terms of pile i
	docs     int
	postings int64
}

// NewPrivateMergeIndex returns an empty index for the given number of workers, with the
// linear-scan merge (private-merge).
func NewPrivateMergeIndex(workers int) *PrivateMergeIndex {
	return &PrivateMergeIndex{n: workers}
}

// NewPrivateMergeHeapIndex returns the same index with the binary min-heap merge (private-merge-heap).
func NewPrivateMergeHeapIndex(workers int) *PrivateMergeIndex {
	return &PrivateMergeIndex{n: workers, heap: true}
}

// pmWorker is one worker's private index plus its private scratch state.
type pmWorker struct {
	x        *PrivateMergeIndex
	doc      *DocPostings
	engine   *QueryEngine
	piles    []map[string]*PostingList // piles[i]: terms of pile i
	docs     int
	postings int64
}

func (x *PrivateMergeIndex) Worker(number int) Worker {
	w := &pmWorker{x: x, doc: NewDocPostings(), engine: NewQueryEngine(), piles: make([]map[string]*PostingList, x.n)}
	for i := range w.piles {
		w.piles[i] = make(map[string]*PostingList)
	}
	x.handles = append(x.handles, w)
	return w
}

func (w *pmWorker) Add(docID int, text string) {
	w.doc.Read(text)
	for k, term := range w.doc.Terms {
		pile := w.piles[PartitionOf(term, w.x.n)]
		list := pile[term]
		if list == nil {
			list = &PostingList{}
			pile[strings.Clone(term)] = list // the term is a substring of the document line: clone it
		}
		list.Append(docID, int(w.doc.Freqs[k])) // IDs of one worker only grow: the list stays sorted
	}
	w.docs++
	w.postings += int64(len(w.doc.Terms))
}

func (w *pmWorker) Query(q *Queries, i int) int { return w.engine.Run(w.x, q, i) }

// Flush has nothing to do: the private index is the buffer.
func (w *pmWorker) Flush() {}

// Finish merges the piles: worker i builds the merged pile i from pile i of every worker.
func (x *PrivateMergeIndex) Finish() {
	x.merged = make([]map[string]*PostingList, x.n)
	RunWorkers(x.n, x.mergePile)
	for _, h := range x.handles {
		x.docs += h.docs
		x.postings += h.postings
		h.piles = nil // the private indexes are no longer needed
	}
}

// mergePile builds merged pile i: for every term, the N-way merge of its lists in pile i of each worker.
func (x *PrivateMergeIndex) mergePile(i int) {
	parts := make(map[string][]*PostingList)
	for _, h := range x.handles {
		for term, list := range h.piles[i] {
			parts[term] = append(parts[term], list)
		}
	}
	result := make(map[string]*PostingList, len(parts))
	for term, lists := range parts {
		if len(lists) == 1 {
			result[term] = lists[0] // one list: reuse it
		} else if x.heap {
			result[term] = mergeListsHeap(lists)
		} else {
			result[term] = mergeLists(lists)
		}
	}
	x.merged[i] = result // each goroutine writes its own slot; Wait publishes it
}

// mergeLists is the N-way merge of sorted lists with disjoint document IDs: repeatedly take the smallest head.
func mergeLists(lists []*PostingList) *PostingList {
	total := 0
	for _, l := range lists {
		total += l.Size()
	}
	out := &PostingList{Docs: make([]int32, 0, total), Freqs: make([]int32, 0, total)}
	at := make([]int, len(lists))
	for n := 0; n < total; n++ {
		best := -1
		for j, l := range lists {
			if at[j] < len(l.Docs) && (best < 0 || l.Docs[at[j]] < lists[best].Docs[at[best]]) {
				best = j
			}
		}
		out.Docs = append(out.Docs, lists[best].Docs[at[best]])
		out.Freqs = append(out.Freqs, lists[best].Freqs[at[best]])
		at[best]++
	}
	return out
}

// mergeListsHeap is the same merge with a binary min-heap: it holds one entry per list that is not
// exhausted, the head document ID (key) and the list it comes from (src), with the smallest key at
// the root. Each posting takes the root, then replaces it by the next head of the same list or, if
// the list is exhausted, by the last entry, and sifts it down: O(log k).
func mergeListsHeap(lists []*PostingList) *PostingList {
	total := 0
	for _, l := range lists {
		total += l.Size()
	}
	out := &PostingList{Docs: make([]int32, 0, total), Freqs: make([]int32, 0, total)}
	at := make([]int, len(lists))
	key := make([]int32, len(lists))
	src := make([]int, len(lists))
	size := 0
	for j, l := range lists {
		if l.Size() > 0 {
			key[size] = l.Docs[0]
			src[size] = j
			size++
		}
	}
	for i := size/2 - 1; i >= 0; i-- { // build the heap
		siftDown(key, src, size, i)
	}
	for n := 0; n < total; n++ {
		j := src[0]
		l := lists[j]
		out.Docs = append(out.Docs, key[0])
		out.Freqs = append(out.Freqs, l.Freqs[at[j]])
		at[j]++
		if at[j] < len(l.Docs) {
			key[0] = l.Docs[at[j]] // the next head of the same list
		} else {
			size-- // list exhausted: the last entry takes the root
			key[0] = key[size]
			src[0] = src[size]
		}
		siftDown(key, src, size, 0)
	}
	return out
}

// siftDown moves the entry at position i down until both its children have larger keys.
func siftDown(key []int32, src []int, size, i int) {
	k0, s0 := key[i], src[i]
	for {
		c := 2*i + 1 // left child
		if c >= size {
			break
		}
		if c+1 < size && key[c+1] < key[c] { // the smaller child
			c++
		}
		if key[c] >= k0 {
			break
		}
		key[i], src[i] = key[c], src[c]
		i = c
	}
	key[i], src[i] = k0, s0
}

func (x *PrivateMergeIndex) Get(term string) *PostingList {
	return x.merged[PartitionOf(term, x.n)][term]
}

func (x *PrivateMergeIndex) Docs() int { return x.docs }

func (x *PrivateMergeIndex) Terms() int {
	n := 0
	for _, m := range x.merged {
		n += len(m)
	}
	return n
}

func (x *PrivateMergeIndex) Postings() int64 { return x.postings }

func (x *PrivateMergeIndex) Each(visit func(term string, list *PostingList)) {
	for _, m := range x.merged {
		for term, list := range m {
			visit(term, list)
		}
	}
}
