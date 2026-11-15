package main

import (
	"math"
	"os"
)

// Go global-lock variant: one lock for the whole index; --batch postings per acquisition.
func main() {
	Run(os.Args[1:], math.MaxInt32, func(a *Args) Index { return NewGlobalLockIndex(a.Batch) }, "batch")
}
