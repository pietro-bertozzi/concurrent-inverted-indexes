package main

// Index is what the shared code needs from an index. A variant implements it with its own
// data structure and concurrency strategy; the harness (Run) never sees the difference.
//
// Every worker goroutine gets its own Worker from Worker(); all the operations that run
// concurrently go through it. The other methods are called by one goroutine only, outside the
// concurrent phases (or, for Get, by a worker that already holds the locks its variant needs).
type Index interface {
	// Worker makes a handle for one worker; called before timing starts.
	Worker() Worker
	// Get returns the posting list of term, sorted by document ID, or nil if the term is absent.
	// No synchronisation of its own: a concurrent variant calls it with the right locks held.
	Get(term string) *PostingList
	// Finish makes the index final (sorting, merging, publishing); timed as finalize_ns.
	Finish()
	// Docs is the number of documents added.
	Docs() int
	// Terms is the number of distinct terms.
	Terms() int
	// Postings is the total number of postings over all terms.
	Postings() int64
	// Each calls visit for every (term, posting list) pair, in any order; used by the dump.
	Each(visit func(term string, list *PostingList))
}

// Worker is the per-worker handle: private buffers live here, so workers never share them.
type Worker interface {
	// Add indexes document docID (one line of the corpus: tokens separated by single spaces).
	Add(docID int, doc string)
	// Query runs query number i of q and returns the number of matching documents.
	Query(q *Queries, i int) int
	// Flush inserts every posting this worker still holds in a private buffer. Called by the
	// worker's own goroutine at the end of every phase (the indexing loop, the timed query or
	// mixed phase).
	Flush()
}
