package main

// Index is what the shared code needs from an index. A variant implements it with its own
// data structure and concurrency strategy; the harness (Run) never sees the difference.
type Index interface {
	// Add indexes document docID (one line of the corpus: tokens separated by single spaces).
	Add(docID int, doc string)
	// Get returns the posting list of term, sorted by document ID, or nil if the term is absent.
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
