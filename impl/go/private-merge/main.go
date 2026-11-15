package main

import (
	"math"
	"os"
)

// Go private-merge variant: a private index per worker, merged in parallel at the end (finalize).
func main() {
	Run(os.Args[1:], math.MaxInt32, func(a *Args) Index { return NewPrivateMergeIndex(a.Threads) })
}
