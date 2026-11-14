package main

import (
	"bufio"
	"fmt"
	"os"
	"strings"
)

// Query kinds.
const (
	QuerySimple = 0
	QueryAnd    = 1
	QueryOr     = 2
)

// Queries is the query file (docs/design.md, section Queries), parsed once:
// "SIMPLE t", "AND a b", "OR a b".
type Queries struct {
	Kind   []int
	First  []string
	Second []string // empty for SIMPLE
}

// Size is the number of queries.
func (q *Queries) Size() int { return len(q.Kind) }

// LoadQueries reads and parses file; the error names the line of a malformed query.
func LoadQueries(file string) (*Queries, error) {
	f, err := os.Open(file)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	q := &Queries{}
	in := bufio.NewReader(f)
	number := 0
	for {
		line, err := in.ReadString('\n')
		if err != nil && line == "" {
			break
		}
		number++
		line = strings.TrimRight(line, "\r\n")
		fields := strings.Split(line, " ")
		kind := 0
		switch fields[0] {
		case "SIMPLE":
			kind = QuerySimple
		case "AND":
			kind = QueryAnd
		case "OR":
			kind = QueryOr
		default:
			return nil, fmt.Errorf("%s line %d: unknown operator '%s'", file, number, fields[0])
		}
		want := 3
		if kind == QuerySimple {
			want = 2
		}
		if len(fields) != want {
			return nil, fmt.Errorf("%s line %d: wrong number of terms for %s", file, number, fields[0])
		}
		q.Kind = append(q.Kind, kind)
		q.First = append(q.First, fields[1])
		if kind == QuerySimple {
			q.Second = append(q.Second, "")
		} else {
			q.Second = append(q.Second, fields[2])
		}
	}
	if q.Size() == 0 {
		return nil, fmt.Errorf("%s has no queries", file)
	}
	return q, nil
}
