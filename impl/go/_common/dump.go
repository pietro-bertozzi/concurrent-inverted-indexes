package main

import (
	"bufio"
	"os"
	"sort"
	"strconv"
)

// WriteDump writes the canonical dump (docs/design.md, section Correctness): terms sorted by
// their UTF-8 bytes (the order of `LC_ALL=C sort`; Go compares strings bytewise), one per
// line, then a tab and the postings "docid:frequency" sorted by document ID, separated by
// single spaces.
func WriteDump(index Index, file string) error {
	type entry struct {
		term string
		list *PostingList
	}
	entries := make([]entry, 0, index.Terms())
	index.Each(func(term string, list *PostingList) { entries = append(entries, entry{term, list}) })
	sort.Slice(entries, func(a, b int) bool { return entries[a].term < entries[b].term })

	f, err := os.Create(file)
	if err != nil {
		return err
	}
	out := bufio.NewWriter(f)
	buf := make([]byte, 0, 256)
	for _, e := range entries {
		buf = append(buf[:0], e.term...)
		buf = append(buf, '\t')
		for i := range e.list.Docs {
			if i > 0 {
				buf = append(buf, ' ')
			}
			buf = strconv.AppendInt(buf, int64(e.list.Docs[i]), 10)
			buf = append(buf, ':')
			buf = strconv.AppendInt(buf, int64(e.list.Freqs[i]), 10)
		}
		buf = append(buf, '\n')
		out.Write(buf)
	}
	if err := out.Flush(); err != nil {
		f.Close()
		return err
	}
	return f.Close()
}
