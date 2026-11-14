package main

import "strings"

// Tokenizer splits a document line into tokens. The corpus has single spaces between tokens
// and no empty tokens (docs/design.md, section Data), so a token is the text between spaces.
// Tokens are substrings of the line: nothing is copied.
type Tokenizer struct {
	doc   string
	start int
	token string
}

// NewTokenizer starts tokenizing doc.
func NewTokenizer(doc string) *Tokenizer { return &Tokenizer{doc: doc} }

// Next moves to the next token; it returns false when there are no more.
func (t *Tokenizer) Next() bool {
	if t.start >= len(t.doc) {
		return false
	}
	end := strings.IndexByte(t.doc[t.start:], ' ')
	if end < 0 {
		end = len(t.doc)
	} else {
		end += t.start
	}
	t.token = t.doc[t.start:end]
	t.start = end + 1
	return true
}

// Token is the current token (valid after Next returned true).
func (t *Tokenizer) Token() string { return t.token }
