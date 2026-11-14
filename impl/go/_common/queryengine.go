package main

// QueryEngine evaluates queries (docs/design.md, section Queries). The result of a query is
// the set of document IDs; the engine returns its size, which the caller adds up so that the
// work cannot be optimised away. AND and OR merge the two sorted lists with two pointers into
// a scratch slice that the engine keeps between calls (one engine per worker).
type QueryEngine struct {
	scratch []int32
}

var emptyList = &PostingList{}

// NewQueryEngine returns an engine with a small scratch slice.
func NewQueryEngine() *QueryEngine { return &QueryEngine{scratch: make([]int32, 1024)} }

// Run runs query number i of q on index and returns the number of matching documents.
func (e *QueryEngine) Run(index Index, q *Queries, i int) int {
	a := list(index, q.First[i])
	if q.Kind[i] == QuerySimple {
		return a.Size()
	}
	b := list(index, q.Second[i])
	if q.Kind[i] == QueryAnd {
		return e.intersect(a, b)
	}
	return e.union(a, b)
}

func list(index Index, term string) *PostingList {
	l := index.Get(term)
	if l == nil {
		return emptyList
	}
	return l
}

// intersect: advance the pointer with the smaller ID, keep equal IDs.
func (e *QueryEngine) intersect(a, b *PostingList) int {
	small := a.Size()
	if b.Size() < small {
		small = b.Size()
	}
	e.reserve(small)
	i, j, n := 0, 0, 0
	for i < a.Size() && j < b.Size() {
		x, y := a.Docs[i], b.Docs[j]
		if x == y {
			e.scratch[n] = x
			n++
			i++
			j++
		} else if x < y {
			i++
		} else {
			j++
		}
	}
	return n
}

// union: always take the smaller ID, an ID present in both is taken once.
func (e *QueryEngine) union(a, b *PostingList) int {
	e.reserve(a.Size() + b.Size())
	i, j, n := 0, 0, 0
	for i < a.Size() && j < b.Size() {
		x, y := a.Docs[i], b.Docs[j]
		if x == y {
			e.scratch[n] = x
			i++
			j++
		} else if x < y {
			e.scratch[n] = x
			i++
		} else {
			e.scratch[n] = y
			j++
		}
		n++
	}
	for i < a.Size() {
		e.scratch[n] = a.Docs[i]
		n++
		i++
	}
	for j < b.Size() {
		e.scratch[n] = b.Docs[j]
		n++
		j++
	}
	return n
}

// reserve makes sure the scratch slice can hold n IDs.
func (e *QueryEngine) reserve(n int) {
	if len(e.scratch) < n {
		size := 2 * len(e.scratch)
		if n > size {
			size = n
		}
		e.scratch = make([]int32, size)
	}
}
