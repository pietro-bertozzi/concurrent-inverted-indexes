package main

import "strings"

// SequentialIndex is one built-in map from term to posting list, no synchronisation.
// Documents are added in increasing ID order, so every posting list is already sorted and
// Finish has nothing to do.
type SequentialIndex struct {
	m        map[string]*PostingList
	docs     int
	postings int64
}

// NewSequentialIndex returns an empty index.
func NewSequentialIndex() *SequentialIndex {
	return &SequentialIndex{m: make(map[string]*PostingList)}
}

func (x *SequentialIndex) Add(docID int, doc string) {
	tokens := NewTokenizer(doc)
	for tokens.Next() {
		term := tokens.Token()
		list := x.m[term]
		if list == nil {
			list = &PostingList{}
			// The token is a substring of the document line: clone it, so that the index
			// owns its keys (as Java's does) and never keeps a document line alive.
			x.m[strings.Clone(term)] = list
		}
		// A term seen earlier in this document is the last posting of its list: count it.
		if n := len(list.Docs); n > 0 && int(list.Docs[n-1]) == docID {
			list.Freqs[n-1]++
		} else {
			list.Append(docID, 1)
			x.postings++
		}
	}
	x.docs++
}

func (x *SequentialIndex) Get(term string) *PostingList { return x.m[term] }

func (x *SequentialIndex) Finish() { /* sorted by construction */ }

func (x *SequentialIndex) Docs() int { return x.docs }

func (x *SequentialIndex) Terms() int { return len(x.m) }

func (x *SequentialIndex) Postings() int64 { return x.postings }

func (x *SequentialIndex) Each(visit func(term string, list *PostingList)) {
	for term, list := range x.m {
		visit(term, list)
	}
}
