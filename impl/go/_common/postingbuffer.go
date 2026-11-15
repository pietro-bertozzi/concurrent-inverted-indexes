package main

// PostingBuffer is a worker's private buffer of postings waiting to be inserted into the shared
// index (docs/experiment-plan.md, Rules every variant follows, Batching). Postings from
// several documents collect here; the variant flushes the buffer, under its lock, when it holds
// batch postings and at the end of every phase. The terms are substrings of document lines:
// the index clones a term when it becomes a new map key.
type PostingBuffer struct {
	Terms []string
	Docs  []int32
	Freqs []int32
}

// Add appends one posting (term, document, frequency).
func (b *PostingBuffer) Add(term string, docID int, freq int32) {
	b.Terms = append(b.Terms, term)
	b.Docs = append(b.Docs, int32(docID))
	b.Freqs = append(b.Freqs, freq)
}

// Size is the number of postings in the buffer.
func (b *PostingBuffer) Size() int { return len(b.Terms) }

// Clear empties the buffer (the slices keep their capacity for reuse).
func (b *PostingBuffer) Clear() {
	b.Terms = b.Terms[:0]
	b.Docs = b.Docs[:0]
	b.Freqs = b.Freqs[:0]
}
