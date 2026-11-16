package main

import (
	"math"
	"os"
)

// Go private-merge-heap variant: private-merge with a binary min-heap in the N-way merge.
func main() {
	Run(os.Args[1:], math.MaxInt32, func(a *Args) Index { return NewPrivateMergeHeapIndex(a.Threads) })
}
