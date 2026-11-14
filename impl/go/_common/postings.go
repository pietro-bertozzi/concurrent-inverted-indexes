package main

// PostingList is a growable array of (document ID, term frequency) pairs, kept as two int32
// slices. Appending uses Go's built-in slice growth.
type PostingList struct {
	Docs  []int32
	Freqs []int32
}

// Size is the number of postings.
func (p *PostingList) Size() int { return len(p.Docs) }

// Append adds a posting; the caller appends in increasing document ID order.
func (p *PostingList) Append(docID int, freq int) {
	p.Docs = append(p.Docs, int32(docID))
	p.Freqs = append(p.Freqs, int32(freq))
}
