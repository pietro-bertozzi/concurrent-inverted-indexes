package main

// DocPostings holds the postings of one document: its distinct terms with their frequencies.
// This is the tokenizing job of the concurrent variants (docs/experiment-plan.md, section Each
// structural dimension): private to a worker, no shared data. One per worker, reused for every
// document. The terms are substrings of the document line (nothing is copied); the index
// clones a term when it becomes a new map key.
type DocPostings struct {
	counts map[string]int32
	Terms  []string // distinct terms of the last document read
	Freqs  []int32  // their frequencies
}

// NewDocPostings returns an empty buffer.
func NewDocPostings() *DocPostings {
	return &DocPostings{counts: make(map[string]int32), Terms: make([]string, 0, 512), Freqs: make([]int32, 0, 512)}
}

// Read replaces the contents with the postings of doc.
func (d *DocPostings) Read(doc string) {
	clear(d.counts)
	tokens := NewTokenizer(doc)
	for tokens.Next() {
		d.counts[tokens.Token()]++
	}
	d.Terms = d.Terms[:0]
	d.Freqs = d.Freqs[:0]
	for term, freq := range d.counts {
		d.Terms = append(d.Terms, term)
		d.Freqs = append(d.Freqs, freq)
	}
}
