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

// Insert adds a posting in its correct place, keeping the list sorted (docs/design.md, section
// Concurrent variants). With dynamic handout documents arrive nearly in order, so the place is
// almost always the end (a plain append) or a few slots before it. The document ID must not
// be in the list yet.
func (p *PostingList) Insert(docID int, freq int) {
	pos := len(p.Docs)
	for pos > 0 && int(p.Docs[pos-1]) > docID { // walk back from the end
		pos--
	}
	p.Append(docID, freq)    // grows the slices if needed
	if pos < len(p.Docs)-1 { // not the end: shift the tail one slot up
		copy(p.Docs[pos+1:], p.Docs[pos:])
		copy(p.Freqs[pos+1:], p.Freqs[pos:])
		p.Docs[pos] = int32(docID)
		p.Freqs[pos] = int32(freq)
	}
}
